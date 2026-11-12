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
    // How far down the estimator's base goes (see "The base field"):
    //   1 = to 8 px cells, then the older fine levels (4 and 2 px) on top
    //   2 = to 8 px cells and no further     3 = to 4 px cells     4 = to 2 px cells
    #ifndef BASE
        #define BASE 4
    #endif
    #if BASE == 2
        #define DLSS5_MV_CELL 8.0
    #elif BASE == 3
        #define DLSS5_MV_CELL 4.0
    #else
        #define DLSS5_MV_CELL 2.0
    #endif
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

uniform bool CAMERA_MODEL <
    ui_category = "Vector refinement (per pixel)";
    ui_label = "Fit the camera's motion and use it where it fits (experimental)";
    ui_tooltip = "When the camera turns, the whole picture moves by one smooth rule. With this on, 900\n"
                 "points of the frame are tracked against the previous frame, one camera motion is fitted\n"
                 "to those that agree (the HUD and moving objects are thrown out), and a pixel takes the\n"
                 "fitted vector wherever that explains it about as well as the provider's own.\n\n"
                 "Measured on tools/fx-testbed's 3D scene with LumeniteFX, 4K: while the camera PANS, twice\n"
                 "as many pixels get a vector that is right to a quarter pixel (58-76% against 15-35%)\n"
                 "and DLAA's output is 6% steadier and 3% closer to its input. While the camera TRAVELS\n"
                 "the fit is refused or, where it is not, wrong: 2-4% less steady. About half a millisecond\n"
                 "a frame at 4K on the test machine (RTX 5090).\n"
                 "Off by default: a gain for footage that mostly pans, not a general one.";
> = false;

uniform bool CAMERA_DEPTH <
    ui_category = "Vector refinement (per pixel)";
    ui_label = "Camera fit: use the depth buffer";
    ui_tooltip = "Adds the terms of a camera that travels (near things shift more than far ones) to the\n"
                 "fit. Needs a depth buffer ReShade can read, so it is of no use on a capture; and on\n"
                 "fx-testbed's scene it did not make the travelling shots better. Off by default.";
> = false;

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
                 "it, IS this pixel and the vector found is within a quarter pixel of it already;\n"
                 "genuinely fractional motion keeps its fraction.";
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

uniform bool RESTORE_HOLD_REPEATS <
    ui_category = "Restore (DLSS5_Feed_Restore technique)";
    ui_label = "Hold repeated frames (30 fps content at 60)";
    ui_tooltip = "A 30 fps game through a 60 Hz capture arrives as every picture twice. A temporal model\n"
                 "does not answer the second showing the way it answered the first -- it goes on settling\n"
                 "-- so the output changes on a frame where nothing in the input did, and the picture\n"
                 "pulses at 30 Hz. With this on, a frame that is the previous one again is shown exactly\n"
                 "as that one was (for up to three repeats in a row; a picture that simply stands still\n"
                 "is let through after that, so it still settles).";
> = true;

uniform int RESTORE_TEXTURE <
    ui_category = "Restore (DLSS5_Feed_Restore technique)";
    ui_type = "combo";
    ui_items = "Off\0Fine texture the model has softened\0";
    ui_label = "Moving fine texture";
    ui_tooltip = "A temporal model resamples its history between pixels wherever the picture moves by a\n"
                 "fraction of a pixel, and a pattern a few pixels a period -- perforated metal, a grille,\n"
                 "a halftone, fabric -- loses a fifth to two fifths of its contrast to that, by an amount\n"
                 "that changes with the fraction: across a floor in perspective that shows as moire, bands\n"
                 "and rings of soft and sharp that crawl as the camera moves. Exact vectors do not help\n"
                 "(measured: they make it a little worse, the history is then used everywhere).\n"
                 "With this on, wherever the output holds clearly less fine contrast than the frame that\n"
                 "went in, on texture (not along a plain edge, which the model may antialias as it likes),\n"
                 "the fine detail is the input's again; everything broader stays the model's.";
> = 1;

uniform float RESTORE_TEXTURE_CONTRAST <
    ui_category = "Restore (DLSS5_Feed_Restore technique)";
    ui_type = "drag"; ui_min = 0.0; ui_max = 0.2; ui_step = 0.002;
    ui_label = "Moving fine texture: minimum contrast";
    ui_tooltip = "5x5 contrast (mean absolute deviation of luma) from which a patch counts as texture worth\n"
                 "putting back. Below it -- film grain, noise, banding dither -- the model's smoothing stays.";
> = 0.012;

uniform int RESTORE_VOLATILE <
    ui_category = "Restore (DLSS5_Feed_Restore technique)";
    ui_type = "combo";
    ui_items = "Off\0Flames, smoke, heat haze: what no motion explains\0";
    ui_label = "Fire, smoke and haze";
    ui_tooltip = "A flame, smoke, mist or the shimmer of hot air has no motion a vector could describe: it\n"
                 "changes shape in place. A temporal model blends it with what it was a frame ago, which\n"
                 "shows as smear and lag -- and no vector, right or wrong, changes that (tools/fx-testbed:\n"
                 "the same error with exact, estimated and zero vectors).\n"
                 "With this on, where the frame changed in a way the motion found does not account for --\n"
                 "or where the motion itself is a jumble from one spot to the next, over an area and not\n"
                 "just along an outline -- the pixel is shown as it came in.";
> = 1;

uniform bool RESTORE_BLACK <
    ui_category = "Restore (DLSS5_Feed_Restore technique)";
    ui_label = "Keep black black";
    ui_tooltip = "A neural pass lifts pure black to a mottled dark grey (measured: 12 8-bit steps on the black\n"
                 "of a logo screen, from the neural pass alone -- DLAA leaves it at 0.1). With this on, a pixel\n"
                 "that came in black goes out black: letterbox bars, a logo's background, a fade to black.\n"
                 "Anything brighter than about one 8-bit step is left to the model.";
> = true;

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

uniform int DEPTH_USE <
    ui_category = "Depth buffer";
    ui_type = "combo";
    ui_items = "Automatic: when there is one and it holds\0Never\0Always (trust ReShade's depth as it is)\0";
    ui_label = "Use the game's depth buffer";
    ui_tooltip = "With a depth buffer the effect knows which pixels belong to the same surface. It then:\n"
                 " - does not let a cell's vector point at a different surface a frame ago;\n"
                 " - gives each pixel the vector of the cells on its own side of an outline;\n"
                 " - gives a pixel that was hidden a frame ago the motion of its own surface, and flags\n"
                 "   it (the depth test below);\n"
                 " - and, if the camera fit is enabled, fits it with the terms of a camera that travels.\n\n"
                 "Automatic checks the depth buffer every frame before believing it: it has to vary across\n"
                 "the screen (a capture, or a game ReShade finds no depth for, has none), and -- moved by\n"
                 "the vectors found from the picture alone -- it has to be last frame's depth on four\n"
                 "fifths of the screen (a buffer that flickers, lags a frame or belongs to something else\n"
                 "is not). After half a second of that it is used; when it stops holding, within a few\n"
                 "frames it is not, and the effect works from the picture alone. ReShade's own depth\n"
                 "settings (reversed, upside down) must be right for the game, as for any depth effect.\n\n"
                 "Measured on tools/fx-testbed: a depth buffer that is empty every other frame, or every\n"
                 "seventh, is never used; with a good one the vectors gain a little and the picture is\n"
                 "unchanged. The camera fit is NOT switched on by this: its vectors are more exact but\n"
                 "never whole pixels, and DLSS resamples its history more with them (the picture came out\n"
                 "further from the input, 0.82 against 0.52 on a pan).";
> = 0;

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
               "Refine: what each pixel took (dark = static lock, green = zero, blue = the provider's vector, cyan = a neighbouring cell, magenta = last frame's vector, yellow = snapped to whole pixels, orange = the fitted camera motion)\0"
               "Vector dump for tools/fx-testbed (10-bit codes: R = 512 + 8 x, G = 512 + 8 y, B = mask)\0"
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
// Is the depth buffer usable? x = for how many frames it has held (see PS_DepthCheck), y = the
// share of the screen where it agreed with last frame's this frame, z = its range.
texture DLSS5_DepthOK     { Width = 1; Height = 1; Format = RGBA16F; };
texture DLSS5_PrevDepthOK { Width = 1; Height = 1; Format = RGBA16F; };
sampler sDLSS5_DepthOK     { Texture = DLSS5_DepthOK;     MinFilter = POINT; MagFilter = POINT; MipFilter = POINT; };
sampler sDLSS5_PrevDepthOK { Texture = DLSS5_PrevDepthOK; MinFilter = POINT; MagFilter = POINT; MipFilter = POINT; };
// (As of last frame: the estimator runs before this frame's check, which needs its vectors.)
bool DepthUsable()  { return DEPTH_USE == 2 || (DEPTH_USE == 0 && tex2Dlod(sDLSS5_PrevDepthOK, float4(0.5, 0.5, 0.0, 0.0)).x >= 30.0); }
// The camera fit is on when asked for (CAM_AUTO=1 would switch it on with the depth buffer: see
// DEPTH_USE for why that is not the default); its depth terms are on by themselves once the
// depth buffer holds.
#ifndef CAM_AUTO
    #define CAM_AUTO 0
#endif
bool CamModelOn()   { return CAMERA_MODEL || (CAM_AUTO && DepthUsable()); }
bool CamDepthOn()   { return CAMERA_DEPTH || GEOM_ENABLE || DepthUsable(); }
// Two depths (linear) of the same surface? 1 = yes, 0 = no, by how far apart they are in proportion.
float SameSurface(float a, float b) { return 1.0 - smoothstep(0.08, 0.25, abs(a - b) / max(min(a, b), 1e-5)); }
// The same for a point and the one opposite it across the middle: on one flat surface, however
// steeply it runs away from the camera, 1 / depth changes by the same amount on either side.
// (A floor seen at a shallow angle doubles its depth within a few cells; judged by depth alone
// it was a staircase of "different surfaces", and its cells stopped agreeing with each other.)
// dc, da, db: linear depth at the middle, at the point, at the point opposite.
float OnSurface(float dc, float da, float db)
{
    const float zc = 1.0 / max(dc, 1e-6), za = 1.0 / max(da, 1e-6), zb = 1.0 / max(db, 1e-6);
    return max(1.0 - smoothstep(0.04, 0.12, abs(za + zb - 2.0 * zc) / zc), SameSurface(dc, da));
}
sampler sDLSS5_PrevMV    { Texture = DLSS5_PrevMV;    AddressU = Clamp; AddressV = Clamp; MinFilter = POINT;  MagFilter = POINT;  MipFilter = POINT; };
// For DLSS5_Feed_Restore. Hud: how sure it is that a pixel is a still element over a moving
// picture (0..1, built up over frames, see the feed pass); PrevHud: last frame's. Before: the
// frame as it came in, kept in the back buffer's own format so that nothing is lost in it.
// x: the share of the frame the provider reports as moving (a 16x9 probe); y: how many frames
// in a row have been the previous frame again (0 = this one is new); z: the share of 8x8 blocks
// that changed. PrevGlobal: last frame's, for the count.
texture DLSS5_Global     { Width = 1; Height = 1; Format = RGBA16F; };
texture DLSS5_PrevGlobal { Width = 1; Height = 1; Format = RGBA16F; };
sampler sDLSS5_Global     { Texture = DLSS5_Global;     MinFilter = POINT; MagFilter = POINT; MipFilter = POINT; };
sampler sDLSS5_PrevGlobal { Texture = DLSS5_PrevGlobal; MinFilter = POINT; MagFilter = POINT; MipFilter = POINT; };
// 1 where anything in an 8x8 block changed since the last frame, 0 where nothing did; its small
// mips say how much of the frame that is.
texture DLSS5_Changed  { Width = BUFFER_WIDTH / 8; Height = BUFFER_HEIGHT / 8; Format = R16F; MipLevels = 7; };
sampler sDLSS5_Changed { Texture = DLSS5_Changed; AddressU = Clamp; AddressV = Clamp; MinFilter = LINEAR; MagFilter = LINEAR; MipFilter = POINT; };
texture DLSS5_Hud     { Width = BUFFER_WIDTH; Height = BUFFER_HEIGHT; Format = R8; };
// Where the picture is doing something no motion explains (a flame, smoke, the shimmer of hot
// air): see PS_MotionVectors and PS_Restore.
texture DLSS5_Vol     { Width = BUFFER_WIDTH; Height = BUFFER_HEIGHT; Format = R8; };
sampler sDLSS5_Vol     { Texture = DLSS5_Vol;     AddressU = Clamp; AddressV = Clamp; MinFilter = LINEAR; MagFilter = LINEAR; MipFilter = POINT; };
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
// What was shown last frame, after everything: a repeated frame is shown as this again.
#if BUFFER_COLOR_BIT_DEPTH == 16
    texture DLSS5_LastOut { Width = BUFFER_WIDTH; Height = BUFFER_HEIGHT; Format = RGBA16F; };
#elif BUFFER_COLOR_BIT_DEPTH == 10
    texture DLSS5_LastOut { Width = BUFFER_WIDTH; Height = BUFFER_HEIGHT; Format = RGB10A2; };
#else
    texture DLSS5_LastOut { Width = BUFFER_WIDTH; Height = BUFFER_HEIGHT; Format = RGBA8; };
#endif
sampler sDLSS5_LastOut { Texture = DLSS5_LastOut; AddressU = Clamp; AddressV = Clamp; MinFilter = POINT; MagFilter = POINT; MipFilter = POINT; };
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
// The estimator's own picture pyramid: this frame's luma and the last one's, each level half
// the one before, made with a 1-3-3-1 filter on each axis instead of the 2x2 box a mip chain is
// built with. The box lets a third of what is just too fine for the next level through, and
// there it comes back as something coarse that was never in the picture -- and that MOVES, by
// a multiple of the real motion and often the other way: brickwork 19 px a course, seen in
// 16 px cells, reads as a 100 px pattern travelling at -5 times the speed of the wall. Every
// level matched that faithfully. The 1-3-3-1 filter leaves 1.5% of it where the box left 25%.
// This frame's levels carry a second channel: how much the cell changed since the last frame,
// in place -- what "did not move" leaves unexplained there.
texture DLSS5_CurL1 { Width = BUFFER_WIDTH / 2; Height = BUFFER_HEIGHT / 2; Format = RG16F; };
sampler sDLSS5_CurL1 { Texture = DLSS5_CurL1; AddressU = Clamp; AddressV = Clamp; MinFilter = LINEAR; MagFilter = LINEAR; MipFilter = POINT; };
texture DLSS5_PrevL1 { Width = BUFFER_WIDTH / 2; Height = BUFFER_HEIGHT / 2; Format = R16F; };
sampler sDLSS5_PrevL1 { Texture = DLSS5_PrevL1; AddressU = Clamp; AddressV = Clamp; MinFilter = LINEAR; MagFilter = LINEAR; MipFilter = POINT; };
texture DLSS5_CurL2 { Width = BUFFER_WIDTH / 4; Height = BUFFER_HEIGHT / 4; Format = RG16F; };
sampler sDLSS5_CurL2 { Texture = DLSS5_CurL2; AddressU = Clamp; AddressV = Clamp; MinFilter = LINEAR; MagFilter = LINEAR; MipFilter = POINT; };
texture DLSS5_PrevL2 { Width = BUFFER_WIDTH / 4; Height = BUFFER_HEIGHT / 4; Format = R16F; };
sampler sDLSS5_PrevL2 { Texture = DLSS5_PrevL2; AddressU = Clamp; AddressV = Clamp; MinFilter = LINEAR; MagFilter = LINEAR; MipFilter = POINT; };
texture DLSS5_CurL3 { Width = BUFFER_WIDTH / 8; Height = BUFFER_HEIGHT / 8; Format = RG16F; };
sampler sDLSS5_CurL3 { Texture = DLSS5_CurL3; AddressU = Clamp; AddressV = Clamp; MinFilter = LINEAR; MagFilter = LINEAR; MipFilter = POINT; };
texture DLSS5_PrevL3 { Width = BUFFER_WIDTH / 8; Height = BUFFER_HEIGHT / 8; Format = R16F; };
sampler sDLSS5_PrevL3 { Texture = DLSS5_PrevL3; AddressU = Clamp; AddressV = Clamp; MinFilter = LINEAR; MagFilter = LINEAR; MipFilter = POINT; };
texture DLSS5_CurL4 { Width = BUFFER_WIDTH / 16; Height = BUFFER_HEIGHT / 16; Format = RG16F; };
sampler sDLSS5_CurL4 { Texture = DLSS5_CurL4; AddressU = Clamp; AddressV = Clamp; MinFilter = LINEAR; MagFilter = LINEAR; MipFilter = POINT; };
texture DLSS5_PrevL4 { Width = BUFFER_WIDTH / 16; Height = BUFFER_HEIGHT / 16; Format = R16F; };
sampler sDLSS5_PrevL4 { Texture = DLSS5_PrevL4; AddressU = Clamp; AddressV = Clamp; MinFilter = LINEAR; MagFilter = LINEAR; MipFilter = POINT; };
texture DLSS5_CurL5 { Width = BUFFER_WIDTH / 32; Height = BUFFER_HEIGHT / 32; Format = RG16F; };
sampler sDLSS5_CurL5 { Texture = DLSS5_CurL5; AddressU = Clamp; AddressV = Clamp; MinFilter = LINEAR; MagFilter = LINEAR; MipFilter = POINT; };
texture DLSS5_PrevL5 { Width = BUFFER_WIDTH / 32; Height = BUFFER_HEIGHT / 32; Format = R16F; };
sampler sDLSS5_PrevL5 { Texture = DLSS5_PrevL5; AddressU = Clamp; AddressV = Clamp; MinFilter = LINEAR; MagFilter = LINEAR; MipFilter = POINT; };
texture DLSS5_CurL6 { Width = BUFFER_WIDTH / 64; Height = BUFFER_HEIGHT / 64; Format = RG16F; };
sampler sDLSS5_CurL6 { Texture = DLSS5_CurL6; AddressU = Clamp; AddressV = Clamp; MinFilter = LINEAR; MagFilter = LINEAR; MipFilter = POINT; };
texture DLSS5_PrevL6 { Width = BUFFER_WIDTH / 64; Height = BUFFER_HEIGHT / 64; Format = R16F; };
sampler sDLSS5_PrevL6 { Texture = DLSS5_PrevL6; AddressU = Clamp; AddressV = Clamp; MinFilter = LINEAR; MagFilter = LINEAR; MipFilter = POINT; };
// Every level's motion equations (see FlowEq / FlowRelax): A = gxx, gxy, gyy and the x of the
// right-hand side, B = its y. St: the field being relaxed -- xy the correction to the coarser
// level's field (in cells), z the cell's luma, w 1 for a cell that stands still.
texture DLSS5_FlowEqA1 { Width = BUFFER_WIDTH / 2; Height = BUFFER_HEIGHT / 2; Format = RGBA32F; };
sampler sDLSS5_FlowEqA1 { Texture = DLSS5_FlowEqA1; AddressU = Clamp; AddressV = Clamp; MinFilter = POINT; MagFilter = POINT; MipFilter = POINT; };
texture DLSS5_FlowEqB1 { Width = BUFFER_WIDTH / 2; Height = BUFFER_HEIGHT / 2; Format = RG32F; };
sampler sDLSS5_FlowEqB1 { Texture = DLSS5_FlowEqB1; AddressU = Clamp; AddressV = Clamp; MinFilter = POINT; MagFilter = POINT; MipFilter = POINT; };
texture DLSS5_FlowStA1 { Width = BUFFER_WIDTH / 2; Height = BUFFER_HEIGHT / 2; Format = RGBA16F; };
sampler sDLSS5_FlowStA1 { Texture = DLSS5_FlowStA1; AddressU = Clamp; AddressV = Clamp; MinFilter = POINT; MagFilter = POINT; MipFilter = POINT; };
texture DLSS5_FlowStB1 { Width = BUFFER_WIDTH / 2; Height = BUFFER_HEIGHT / 2; Format = RGBA16F; };
sampler sDLSS5_FlowStB1 { Texture = DLSS5_FlowStB1; AddressU = Clamp; AddressV = Clamp; MinFilter = POINT; MagFilter = POINT; MipFilter = POINT; };
texture DLSS5_FlowEqA2 { Width = BUFFER_WIDTH / 4; Height = BUFFER_HEIGHT / 4; Format = RGBA32F; };
sampler sDLSS5_FlowEqA2 { Texture = DLSS5_FlowEqA2; AddressU = Clamp; AddressV = Clamp; MinFilter = POINT; MagFilter = POINT; MipFilter = POINT; };
texture DLSS5_FlowEqB2 { Width = BUFFER_WIDTH / 4; Height = BUFFER_HEIGHT / 4; Format = RG32F; };
sampler sDLSS5_FlowEqB2 { Texture = DLSS5_FlowEqB2; AddressU = Clamp; AddressV = Clamp; MinFilter = POINT; MagFilter = POINT; MipFilter = POINT; };
texture DLSS5_FlowStA2 { Width = BUFFER_WIDTH / 4; Height = BUFFER_HEIGHT / 4; Format = RGBA16F; };
sampler sDLSS5_FlowStA2 { Texture = DLSS5_FlowStA2; AddressU = Clamp; AddressV = Clamp; MinFilter = POINT; MagFilter = POINT; MipFilter = POINT; };
texture DLSS5_FlowStB2 { Width = BUFFER_WIDTH / 4; Height = BUFFER_HEIGHT / 4; Format = RGBA16F; };
sampler sDLSS5_FlowStB2 { Texture = DLSS5_FlowStB2; AddressU = Clamp; AddressV = Clamp; MinFilter = POINT; MagFilter = POINT; MipFilter = POINT; };
texture DLSS5_FlowEqA3 { Width = BUFFER_WIDTH / 8; Height = BUFFER_HEIGHT / 8; Format = RGBA32F; };
sampler sDLSS5_FlowEqA3 { Texture = DLSS5_FlowEqA3; AddressU = Clamp; AddressV = Clamp; MinFilter = POINT; MagFilter = POINT; MipFilter = POINT; };
texture DLSS5_FlowEqB3 { Width = BUFFER_WIDTH / 8; Height = BUFFER_HEIGHT / 8; Format = RG32F; };
sampler sDLSS5_FlowEqB3 { Texture = DLSS5_FlowEqB3; AddressU = Clamp; AddressV = Clamp; MinFilter = POINT; MagFilter = POINT; MipFilter = POINT; };
texture DLSS5_FlowStA3 { Width = BUFFER_WIDTH / 8; Height = BUFFER_HEIGHT / 8; Format = RGBA16F; };
sampler sDLSS5_FlowStA3 { Texture = DLSS5_FlowStA3; AddressU = Clamp; AddressV = Clamp; MinFilter = POINT; MagFilter = POINT; MipFilter = POINT; };
texture DLSS5_FlowStB3 { Width = BUFFER_WIDTH / 8; Height = BUFFER_HEIGHT / 8; Format = RGBA16F; };
sampler sDLSS5_FlowStB3 { Texture = DLSS5_FlowStB3; AddressU = Clamp; AddressV = Clamp; MinFilter = POINT; MagFilter = POINT; MipFilter = POINT; };
texture DLSS5_FlowEqA4 { Width = BUFFER_WIDTH / 16; Height = BUFFER_HEIGHT / 16; Format = RGBA32F; };
sampler sDLSS5_FlowEqA4 { Texture = DLSS5_FlowEqA4; AddressU = Clamp; AddressV = Clamp; MinFilter = POINT; MagFilter = POINT; MipFilter = POINT; };
texture DLSS5_FlowEqB4 { Width = BUFFER_WIDTH / 16; Height = BUFFER_HEIGHT / 16; Format = RG32F; };
sampler sDLSS5_FlowEqB4 { Texture = DLSS5_FlowEqB4; AddressU = Clamp; AddressV = Clamp; MinFilter = POINT; MagFilter = POINT; MipFilter = POINT; };
texture DLSS5_FlowStA4 { Width = BUFFER_WIDTH / 16; Height = BUFFER_HEIGHT / 16; Format = RGBA16F; };
sampler sDLSS5_FlowStA4 { Texture = DLSS5_FlowStA4; AddressU = Clamp; AddressV = Clamp; MinFilter = POINT; MagFilter = POINT; MipFilter = POINT; };
texture DLSS5_FlowStB4 { Width = BUFFER_WIDTH / 16; Height = BUFFER_HEIGHT / 16; Format = RGBA16F; };
sampler sDLSS5_FlowStB4 { Texture = DLSS5_FlowStB4; AddressU = Clamp; AddressV = Clamp; MinFilter = POINT; MagFilter = POINT; MipFilter = POINT; };
texture DLSS5_FlowEqA5 { Width = BUFFER_WIDTH / 32; Height = BUFFER_HEIGHT / 32; Format = RGBA32F; };
sampler sDLSS5_FlowEqA5 { Texture = DLSS5_FlowEqA5; AddressU = Clamp; AddressV = Clamp; MinFilter = POINT; MagFilter = POINT; MipFilter = POINT; };
texture DLSS5_FlowEqB5 { Width = BUFFER_WIDTH / 32; Height = BUFFER_HEIGHT / 32; Format = RG32F; };
sampler sDLSS5_FlowEqB5 { Texture = DLSS5_FlowEqB5; AddressU = Clamp; AddressV = Clamp; MinFilter = POINT; MagFilter = POINT; MipFilter = POINT; };
texture DLSS5_FlowStA5 { Width = BUFFER_WIDTH / 32; Height = BUFFER_HEIGHT / 32; Format = RGBA16F; };
sampler sDLSS5_FlowStA5 { Texture = DLSS5_FlowStA5; AddressU = Clamp; AddressV = Clamp; MinFilter = POINT; MagFilter = POINT; MipFilter = POINT; };
texture DLSS5_FlowStB5 { Width = BUFFER_WIDTH / 32; Height = BUFFER_HEIGHT / 32; Format = RGBA16F; };
sampler sDLSS5_FlowStB5 { Texture = DLSS5_FlowStB5; AddressU = Clamp; AddressV = Clamp; MinFilter = POINT; MagFilter = POINT; MipFilter = POINT; };
texture DLSS5_FlowEqA6 { Width = BUFFER_WIDTH / 64; Height = BUFFER_HEIGHT / 64; Format = RGBA32F; };
sampler sDLSS5_FlowEqA6 { Texture = DLSS5_FlowEqA6; AddressU = Clamp; AddressV = Clamp; MinFilter = POINT; MagFilter = POINT; MipFilter = POINT; };
texture DLSS5_FlowEqB6 { Width = BUFFER_WIDTH / 64; Height = BUFFER_HEIGHT / 64; Format = RG32F; };
sampler sDLSS5_FlowEqB6 { Texture = DLSS5_FlowEqB6; AddressU = Clamp; AddressV = Clamp; MinFilter = POINT; MagFilter = POINT; MipFilter = POINT; };
texture DLSS5_FlowStA6 { Width = BUFFER_WIDTH / 64; Height = BUFFER_HEIGHT / 64; Format = RGBA16F; };
sampler sDLSS5_FlowStA6 { Texture = DLSS5_FlowStA6; AddressU = Clamp; AddressV = Clamp; MinFilter = POINT; MagFilter = POINT; MipFilter = POINT; };
texture DLSS5_FlowStB6 { Width = BUFFER_WIDTH / 64; Height = BUFFER_HEIGHT / 64; Format = RGBA16F; };
sampler sDLSS5_FlowStB6 { Texture = DLSS5_FlowStB6; AddressU = Clamp; AddressV = Clamp; MinFilter = POINT; MagFilter = POINT; MipFilter = POINT; };
// Each coarser level as matched, before its cells are made to agree (FlowAgree).
texture DLSS5_Flow2Raw { Width = BUFFER_WIDTH / 4; Height = BUFFER_HEIGHT / 4; Format = RGBA16F; };
sampler sDLSS5_Flow2Raw { Texture = DLSS5_Flow2Raw; AddressU = Clamp; AddressV = Clamp; MinFilter = POINT; MagFilter = POINT; MipFilter = POINT; };
texture DLSS5_Flow3Raw { Width = BUFFER_WIDTH / 8; Height = BUFFER_HEIGHT / 8; Format = RGBA16F; };
sampler sDLSS5_Flow3Raw { Texture = DLSS5_Flow3Raw; AddressU = Clamp; AddressV = Clamp; MinFilter = POINT; MagFilter = POINT; MipFilter = POINT; };
texture DLSS5_Flow4Raw { Width = BUFFER_WIDTH / 16; Height = BUFFER_HEIGHT / 16; Format = RGBA16F; };
sampler sDLSS5_Flow4Raw { Texture = DLSS5_Flow4Raw; AddressU = Clamp; AddressV = Clamp; MinFilter = POINT; MagFilter = POINT; MipFilter = POINT; };
texture DLSS5_Flow5Raw { Width = BUFFER_WIDTH / 32; Height = BUFFER_HEIGHT / 32; Format = RGBA16F; };
sampler sDLSS5_Flow5Raw { Texture = DLSS5_Flow5Raw; AddressU = Clamp; AddressV = Clamp; MinFilter = POINT; MagFilter = POINT; MipFilter = POINT; };
texture DLSS5_Flow6Raw { Width = BUFFER_WIDTH / 64; Height = BUFFER_HEIGHT / 64; Format = RGBA16F; };
sampler sDLSS5_Flow6Raw { Texture = DLSS5_Flow6Raw; AddressU = Clamp; AddressV = Clamp; MinFilter = POINT; MagFilter = POINT; MipFilter = POINT; };
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
sampler sDLSS5_Flow2L { Texture = DLSS5_Flow2; AddressU = Clamp; AddressV = Clamp; MinFilter = LINEAR; MagFilter = LINEAR; MipFilter = POINT; };
sampler sDLSS5_Flow3L { Texture = DLSS5_Flow3; AddressU = Clamp; AddressV = Clamp; MinFilter = LINEAR; MagFilter = LINEAR; MipFilter = POINT; };
sampler sDLSS5_Flow4L { Texture = DLSS5_Flow4; AddressU = Clamp; AddressV = Clamp; MinFilter = LINEAR; MagFilter = LINEAR; MipFilter = POINT; };
sampler sDLSS5_Flow5L { Texture = DLSS5_Flow5; AddressU = Clamp; AddressV = Clamp; MinFilter = LINEAR; MagFilter = LINEAR; MipFilter = POINT; };
sampler sDLSS5_Flow6L { Texture = DLSS5_Flow6; AddressU = Clamp; AddressV = Clamp; MinFilter = LINEAR; MagFilter = LINEAR; MipFilter = POINT; };
// Last frame's field (its 16 px level), kept for the coarsest level to start from: see PS_Flow6.
texture DLSS5_FlowKeep { Width = BUFFER_WIDTH / 16; Height = BUFFER_HEIGHT / 16; Format = RGBA16F; };
sampler sDLSS5_FlowKeep { Texture = DLSS5_FlowKeep; AddressU = Clamp; AddressV = Clamp; MinFilter = LINEAR; MagFilter = LINEAR; MipFilter = POINT; };
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

// The same for a PIXEL, with a depth buffer (see DEPTH_USE): from the cells around it that are
// on its own surface. Read plainly, a field of cells is a blend of the two motions for a cell's
// width either side of every outline -- and the pixels just inside the outline of something
// that moves, or of a railing in front of a floor, are exactly where a temporal pass shows a
// wrong vector as a trail. With depth each pixel knows which side it is on.
// (Fields that are coarser than the picture only: the built-in estimator's, LumeniteFX's.)
#ifndef SURF_MV
    #define SURF_MV 1
#endif
float2 ProviderSurfaceMV(float2 uv)
{
#if (DLSS5_MV_BUILTIN || DLSS5_MV_LOWRES) && SURF_MV
    if (!DepthUsable()) return ProviderMV(uv);
  #if DLSS5_MV_BUILTIN
    const float2 gs = BUFFER_SCREEN_SIZE / 2.0;
  #else
    const float2 gs = BUFFER_SCREEN_SIZE / 8.0;
  #endif
    const float2 g = uv * gs - 0.5, i0 = floor(g), f = g - i0;
    const float  dc = ReShade::GetLinearizedDepth(uv);
    float2 sum = 0.0;
    float  ws  = 0.0;
    [unroll] for (int k = 0; k < 4; ++k)
    {
        const float2 o   = float2(k % 2, k / 2);
        const float2 cuv = (i0 + o + 0.5) / gs;
        const float  w   = (o.x > 0.5 ? f.x : 1.0 - f.x) * (o.y > 0.5 ? f.y : 1.0 - f.y) * (SameSurface(dc, ReShade::GetLinearizedDepth(cuv)) + 1e-3);
        sum += w * tex2Dlod(sDLSS5_ProviderMVPoint, float4(cuv, 0.0, 0.0)).xy;
        ws  += w;
    }
  #if DLSS5_MV_BUILTIN
    return sum / ws * BUFFER_PIXEL_SIZE;
  #else
    return sum / ws;
  #endif
#else
    return ProviderMV(uv);
#endif
}

// ---------------------------------------------------------------------------------------------
// Camera model. Screen position x, y in -0.5..0.5, inverse-depth term w = s / (depth + s).
// Basis (9 terms):  1, x, y, x^2, xy, y^2, w, xw, yw  -- the small-rotation flow field of a
// pinhole camera is quadratic in the image position, and translation adds terms in 1/Z.
// Both flow components share the basis; the fit solves them together (two right-hand sides).
// ---------------------------------------------------------------------------------------------

#define DLSS5_BASIS(B, x, y, w) \
    B[0] = 1.0; B[1] = x; B[2] = y; B[3] = x * x; B[4] = x * y; B[5] = y * y; B[6] = w; B[7] = x * w; B[8] = y * w;

// Without a depth buffer worth the name (a capture, a game ReShade cannot read depth from) the
// three parallax terms are switched off and the model is what a camera that only turns and
// zooms does to a picture.
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

// The model's depth term: the RAW depth buffer value. A perspective projection writes
// a + b / distance into it -- whatever its near and far planes, reversed or not -- and the
// shift of a point under a travelling camera goes with 1 / distance, so with the constant and
// the two position terms already in the basis the raw value spans exactly what is needed, with
// nothing to configure. (It used to be s / (linear depth + s): bounded, but not proportional
// to 1 / distance where it matters, close to the camera -- and dependent on the far plane
// ReShade was told. Fitted with that, a travelling shot was no better than without depth.)
// The sky, at the far plane, is then simply "infinitely far": the camera's turning and nothing else.
// The value is brought to 0..1 over the fitted samples (PS_FitSolve keeps the range in Cam4.zw)
// to keep the solve well conditioned; that is an affine change and alters nothing else.
float ParallaxRaw(float2 uv) { return CamDepthOn() ? RawDepth(uv) : 0.0; }

// The model's predicted delta-UV at uv for linear depth d.
float2 PredictMV(float2 uv, float d)
{
    const float4 c = float4(0.5, 0.5, 0.0, 0.0);
    const float4 p0 = tex2Dlod(sDLSS5_Cam0, c), p1 = tex2Dlod(sDLSS5_Cam1, c), p2 = tex2Dlod(sDLSS5_Cam2, c);
    const float4 p3 = tex2Dlod(sDLSS5_Cam3, c), p4 = tex2Dlod(sDLSS5_Cam4, c);
    const float x = uv.x - 0.5, y = uv.y - 0.5, w = (ParallaxRaw(uv) - p4.z) * p4.w;
    float B[9]; DLSS5_BASIS(B, x, y, w)
    // u: p0.xyzw p1.xyzw p2.x     v: p2.yzw p3.xyzw p4.xy
    const float u = p0.x * B[0] + p0.y * B[1] + p0.z * B[2] + p0.w * B[3] + p1.x * B[4] + p1.y * B[5] + p1.z * B[6] + p1.w * B[7] + p2.x * B[8];
    const float v = p2.y * B[0] + p2.z * B[1] + p2.w * B[2] + p3.x * B[3] + p3.y * B[4] + p3.z * B[5] + p3.w * B[6] + p4.x * B[7] + p4.y * B[8];
    return float2(u, v);
}

bool FitIsUsable()
{
    // x = inlier share of the whole grid, y = rms px, z = samples used, w = samples offered
    const float4 s = tex2Dlod(sDLSS5_Cam5, float4(0.5, 0.5, 0.0, 0.0));
    // For the experimental geometry vectors: a quarter of the grid. For the per-pixel stage's
    // camera vector, whose samples are only the trackable ones: two thirds of those -- one
    // motion has to explain that much of what can be followed at all, or there is no "the
    // camera". (It is a blunt test. With some hundred samples a pan scores 0.7-0.9 and a
    // camera travelling past things at different distances 0.45-0.7. At 0.65 most of
    // fx-testbed's travelling frames are refused and those shots come out 2-3% less steady
    // than without the fit; at 0.5 it is 5-7%, with no test at all 9-10%.)
#ifndef FITK
    #define FITK 0.65
#endif
    return s.z >= 40.0 && s.z >= FITK * s.w;
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

// The estimator's own noise floor: a third of FLOW_NOISE. FLOW_NOISE is sized for deciding, per
// pixel, that something did NOT move; used here as well it made every weakly textured surface
// that moves slowly -- a floor, a plain wall -- tie with "did not move", which has the head
// start. On tools/fx-testbed's 3D scene a third halves the error on such surfaces (8.6 -> 4.1 px
// under a 38 px pan, 6.2 -> 3.4 under a strafe) and leaves the HUD at zero.
#define DLSS5_FLOW_FLOOR (FLOW_NOISE / 3.0)

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
// long at the finest level, and came out well short.
#define DLSS5_FLOW_TRY(V) \
    { const float2 v_ = (V); \
      if (any(abs(v_ - best) > 0.125 * cell)) { \
          const float e_ = FlowCost(c, m, cw, mw, uv, v_, t, wide, sP, sPw); \
          const float n_ = length(v_ - ref) / cell; \
          const float w_ = e_ * (1.0 + 0.04 * n_) + 0.25 * DLSS5_FLOW_FLOOR * n_; \
          if (w_ < best_cost) { best_cost = w_; best_raw = e_; best = v_; } } }

// How much of a patch's structure runs in more than one direction: the smallest, over the four
// directions a 3x3 has (along rows, along columns, along either diagonal), of the mean step
// between neighbours. A line has none along itself, however strong it is -- and a patch that
// is only a line cannot tell whether it moved along it. (The courtyard's floor under a strafe:
// the joints that run with the motion all said "did not move", with a perfect score.)
float P9Corner(P9 p)
{
    const float h  = (abs(p.r0.x - p.r0.y) + abs(p.r0.y - p.r0.z) + abs(p.r1.x - p.r1.y) + abs(p.r1.y - p.r1.z) + abs(p.r2.x - p.r2.y) + abs(p.r2.y - p.r2.z)) / 6.0;
    const float v  = (Sum3(abs(p.r0 - p.r1)) + Sum3(abs(p.r1 - p.r2))) / 6.0;
    const float d1 = (abs(p.r0.x - p.r1.y) + abs(p.r1.y - p.r2.z) + abs(p.r0.y - p.r1.z) + abs(p.r1.x - p.r2.y)) / 4.0;
    const float d2 = (abs(p.r0.z - p.r1.y) + abs(p.r1.y - p.r2.x) + abs(p.r0.y - p.r1.x) + abs(p.r1.z - p.r2.y)) / 4.0;
    return min(min(h, v), min(d1, d2));
}

// How far a cell's vector can be trusted, 0..1: its patch pins motion down in every direction
// (see P9Corner) and the match explains it (what is left over is a small part of its contrast).
float FlowTrust(float corner, float contrast, float cost)
{
    return smoothstep(4.0 * DLSS5_FLOW_FLOOR, 12.0 * DLSS5_FLOW_FLOOR, corner) * (1.0 - smoothstep(0.3, 0.6, cost / max(contrast, 1e-6)));
}

// Two layers. A window of a game's picture often holds something that stands still over
// something that moves: a HUD counter, a caption, the edge of a translucent panel with the
// world showing through it. Its edges are the strongest thing in the window and they match
// perfectly at "did not move", so they used to decide the window: every still element was
// ringed by a hundred pixels of dead vectors while the camera moved behind it. (Leaving out
// the samples that did not change is not enough: under a translucent panel everything changes
// and the panel's edge still stands.)
// So each sample may be explained by the candidate OR by standing still, whichever does it
// better, and costs what the better one leaves. A still edge then costs every candidate the
// same nothing, and the candidates are told apart by the samples that moved. For "did not
// move" itself, and for a window with nothing still in it, this is P9Cost as it was.
//   r0: the window's samples of the pyramid's second channel -- |this frame - last frame| in place.
P9 Patch9Y(sampler2D s, float2 uv, float2 t)
{
    P9 p;
    p.r0 = float3(abs(tex2Dlod(s, float4(uv + float2(-t.x, -t.y), 0.0, 0.0)).y), abs(tex2Dlod(s, float4(uv + float2(0.0, -t.y), 0.0, 0.0)).y), abs(tex2Dlod(s, float4(uv + float2(t.x, -t.y), 0.0, 0.0)).y));
    p.r1 = float3(abs(tex2Dlod(s, float4(uv + float2(-t.x,  0.0), 0.0, 0.0)).y), abs(tex2Dlod(s, float4(uv,                       0.0, 0.0)).y), abs(tex2Dlod(s, float4(uv + float2(t.x,  0.0), 0.0, 0.0)).y));
    p.r2 = float3(abs(tex2Dlod(s, float4(uv + float2(-t.x,  t.y), 0.0, 0.0)).y), abs(tex2Dlod(s, float4(uv + float2(0.0,  t.y), 0.0, 0.0)).y), abs(tex2Dlod(s, float4(uv + float2(t.x,  t.y), 0.0, 0.0)).y));
    return p;
}
#ifndef DPEN
    #define DPEN 0.25
#endif
float P9CostL(P9 c, P9 p, P9 rs)
{
    // (Only where standing still explains a sample nearly: a sample that changed a lot is
    // moving, and letting "still" cap its cost as well made every wrong candidate on a moving
    // pattern -- a checker, a grille -- as cheap as the right one.)
    P9 r0;
#ifndef TLK
    #define TLK 8.0
#endif
    const float cap = TLK * FLOW_NOISE;
    r0.r0 = rs.r0 + step(cap, rs.r0) * 1e3; r0.r1 = rs.r1 + step(cap, rs.r1) * 1e3; r0.r2 = rs.r2 + step(cap, rs.r2) * 1e3;
    // The samples the candidate explains at least as well as standing still does: the ones
    // whose brightness difference the candidate is judged on.
    const float3 g0 = step(abs(c.r0 - p.r0), r0.r0 + 1e-5), g1 = step(abs(c.r1 - p.r1), r0.r1 + 1e-5), g2 = step(abs(c.r2 - p.r2), r0.r2 + 1e-5);
    const float  n  = Sum3(g0) + Sum3(g1) + Sum3(g2);
    const float  d  = n > 0.5 ? (dot(c.r0 - p.r0, g0) + dot(c.r1 - p.r1, g1) + dot(c.r2 - p.r2, g2)) / n : 0.0;
    return (Sum3(min(abs(c.r0 - p.r0 - d), r0.r0)) + Sum3(min(abs(c.r1 - p.r1 - d), r0.r1)) + Sum3(min(abs(c.r2 - p.r2 - d), r0.r2))) / 9.0 + DPEN * abs(d) * n / 9.0;
}

// One level of the pyramid from the one below it: four bilinear taps three quarters of a source
// texel out, which is 1-3-3-1 on each axis.
float PyrDown(sampler2D src, float2 uv, float2 st)
{
    const float2 o = 0.75 * st;
    return 0.25 * (tex2Dlod(src, float4(uv + float2(-o.x, -o.y), 0.0, 0.0)).x + tex2Dlod(src, float4(uv + float2(o.x, -o.y), 0.0, 0.0)).x
                 + tex2Dlod(src, float4(uv + float2(-o.x,  o.y), 0.0, 0.0)).x + tex2Dlod(src, float4(uv + float2(o.x,  o.y), 0.0, 0.0)).x);
}
#ifndef PYR_GAIN
    #define PYR_GAIN 1.0
#endif
#define DLSS5_PYR(N, SRC_C, SRC_P, K) \
    float  PS_PyrP##N(float4 vpos : SV_Position, float2 uv : TEXCOORD) : SV_Target { return PyrDown(SRC_P, uv, BUFFER_PIXEL_SIZE * K) * (K > 1.5 ? PYR_GAIN : 1.0); } \
    float2 PS_PyrC##N(float4 vpos : SV_Position, float2 uv : TEXCOORD) : SV_Target \
    { \
        const float c = PyrDown(SRC_C, uv, BUFFER_PIXEL_SIZE * K) * (K > 1.5 ? PYR_GAIN : 1.0); \
        return float2(c, c - tex2Dlod(sDLSS5_PrevL##N, float4(uv, 0.0, 0.0)).x); \
    }
DLSS5_PYR(1, sDLSS5_CurLuma, sDLSS5_PrevLuma, 1.0)
DLSS5_PYR(2, sDLSS5_CurL1, sDLSS5_PrevL1, 2.0)
DLSS5_PYR(3, sDLSS5_CurL2, sDLSS5_PrevL2, 4.0)
DLSS5_PYR(4, sDLSS5_CurL3, sDLSS5_PrevL3, 8.0)
DLSS5_PYR(5, sDLSS5_CurL4, sDLSS5_PrevL4, 16.0)
DLSS5_PYR(6, sDLSS5_CurL5, sDLSS5_PrevL5, 32.0)

// What a candidate costs: the 3x3 of this level's cells, and (below the coarsest level) the 3x3
// of the next level's around it -- six cells across in all.
float FlowCost(P9 c, P9 m, P9 cw, P9 mw, float2 uv, float2 v, float2 t, bool wide, sampler2D sP, sampler2D sPw)
{
    float e = P9CostL(c, Patch9(sP, uv + v * BUFFER_PIXEL_SIZE, t, 0.0), m);
    if (wide) e = 0.5 * (e + P9CostL(cw, Patch9(sPw, uv + v * BUFFER_PIXEL_SIZE, t * 2.0, 0.0), mw));
    return e;
}

// SymCost on this level's own textures (see there).
float FlowSym(P9 c, P9 m, float2 uv, float2 v, float cell, sampler2D sC, sampler2D sP)
{
    const float2 px = BUFFER_PIXEL_SIZE;
    const float2 vi = round(v / cell) * cell;
    const float2 vf = v - vi;
    if (dot(vf, vf) < 1e-4 * cell * cell) return P9CostL(c, Patch9(sP, uv + vi * px, px * cell, 0.0), m);
    const P9 cs = Patch9(sC, uv - 0.5 * vf * px, px * cell, 0.0);
    return P9CostL(cs, Patch9(sP, uv + (vi + 0.5 * vf) * px, px * cell, 0.0), m);
}

// One gradient step (Lucas-Kanade) on this level: the vector v (pixels) corrected by what a 5x5
// window of cells, read symmetrically like FlowSym, says is still missing. Block matching finds
// the right place to within about a quarter of a cell and no better -- which on a soft texture
// that only shows in 16 to 64 px cells (the grain of a floor, a cloud, a gradient) is several
// pixels, and nothing finer can correct it, because finer cells see nothing there at all.
// A smooth texture is exactly what gradients measure well: to a few hundredths of a cell.
//   lmin: the smaller eigenvalue of the window's gradient matrix, per cell and tap -- how well
//         it pins motion down in its weakest direction (a line: not at all along itself);
//   res, dev: what the corrected vector leaves unexplained, and the window's own contrast.
// Gradients that stayed where they were. A window over a game's picture often holds a still
// edge with a moving picture on both sides of it -- the border of a translucent panel, a HUD
// frame -- and that edge is the strongest gradient in the window. In the sums it says "no
// motion across me", with all its weight, and the cells around every such edge came out with
// the part of their motion ACROSS the edge missing: left of the subtitle box under a dolly
// (+0.6, -17.0) for a true (+14.8, -19.8), above it (-0.1, -1.1) for (0.0, -16.7), fifty to a
// hundred pixels out. A sample that changed as a whole does not give it away (everything under
// a translucent panel changes); but its GRADIENT did not change in place, while the gradients
// of what moves did. So each sample is weighed by how much its gradient changed in place
// against how much the window's gradients did on average: a still edge among moving texture
// falls out, and a window that is all still, or all moving slowly, is left as it is (every
// sample is then alike).
//   g2: the sample's squared gradient, d2: the squared in-place change of it (the gradient of
//   this frame minus last frame, in place); returns the ratio the average is taken of.
#ifndef SGK
    #define SGK 0.25
#endif
float FlowStillRatio(float g2, float d2, float eg) { return d2 / (g2 + eg); }

// Motion, or a change of brightness? A window's sums can be solved either way: for a motion
// alone, or for a motion and a brightness offset (the sums with their means taken off). The
// second never explains less -- but it is blind to the commonest thing in a rendered picture, a
// soft slope sliding along, which IS a change of brightness everywhere in the window. So the
// offset is allowed in proportion to what it buys: nothing where motion alone already explains
// the window (the slope), all of it where motion alone leaves several times more unexplained
// than motion with an offset does (a lamp that flickers on a wall that stands still).
// Returns the share of the means to take off, LKC..1. fl2: the noise floor squared, in the
// units of the sums.
#ifndef LKC
    #define LKC 0.25
#endif
float FlowOffsetShare(float sxx, float sxy, float syy, float sxt, float syt, float stt, float sx, float sy, float st, float n, float fl2)
{
    const float e0  = 1e-3 * (sxx + syy) + n * fl2 * 1e-3;
    const float eu  = stt - ((syy + e0) * sxt * sxt - 2.0 * sxy * sxt * syt + (sxx + e0) * syt * syt) / max((sxx + e0) * (syy + e0) - sxy * sxy, 1e-30);
    const float cxx = sxx - sx * sx / n, cxy = sxy - sx * sy / n, cyy = syy - sy * sy / n;
    const float cxt = sxt - sx * st / n, cyt = syt - sy * st / n, ctt = stt - st * st / n;
    const float ec  = ctt - ((cyy + e0) * cxt * cxt - 2.0 * cxy * cxt * cyt + (cxx + e0) * cyt * cyt) / max((cxx + e0) * (cyy + e0) - cxy * cxy, 1e-30);
    return lerp(LKC, 1.0, smoothstep(2.0, 6.0, max(eu, 0.0) / (max(ec, 0.0) + n * fl2)));
}

float2 FlowTrack(sampler2D sC, sampler2D sP, float2 uv, float2 v, float cell, out float lmin, out float lmax, out float res, out float dev, out float reach)
{
    const float2 px = BUFFER_PIXEL_SIZE;
    const float2 vi = round(v / cell) * cell, vf = v - vi;
    float a[49], b[49], w[49], dd[49];
    float wn = 0.0;
    [loop] for (int k = 0; k < 49; ++k)
    {
        const float2 o  = float2(k % 7 - 3, k / 7 - 3) * cell;
        const float2 ca = tex2Dlod(sC, float4(uv + (o - 0.5 * vf) * px, 0.0, 0.0)).xy;
        a[k] = ca.x; dd[k] = ca.y; w[k] = abs(ca.y); wn += abs(ca.y);
        b[k] = tex2Dlod(sP, float4(uv + (o + vi + 0.5 * vf) * px, 0.0, 0.0)).x;
    }
    // A sample that standing still explains better than the vector does belongs to something
    // else (see P9CostL) and has no say -- unless that leaves next to nothing.
    float cnt = 0.0;
    [loop] for (int k9 = 0; k9 < 49; ++k9)
    {
        w[k9] = abs(b[k9] - a[k9]) <= w[k9] + FLOW_NOISE ? 1.0 : 0.0;
        cnt += w[k9];
    }
#ifndef LKMASK
    #define LKMASK 1
#endif
    const bool all_in = cnt < 10.0 || LKMASK == 0 || (LKMASK == 2 && cell > 6.0);
    // (see FlowStillRatio)
    float sr[49];
    float srm = 0.0;
    [loop] for (int j0 = 1; j0 < 6; ++j0)
    {
        [loop] for (int i0 = 1; i0 < 6; ++i0)
        {
            const int    c0 = j0 * 7 + i0;
            const float2 g0 = 0.5 * float2(a[c0 + 1] - a[c0 - 1], a[c0 + 7] - a[c0 - 7]);
            const float2 d0 = 0.5 * float2(dd[c0 + 1] - dd[c0 - 1], dd[c0 + 7] - dd[c0 - 7]);
            sr[c0] = FlowStillRatio(dot(g0, g0), dot(d0, d0), 4.0 * DLSS5_FLOW_FLOOR * DLSS5_FLOW_FLOOR);
            srm += sr[c0] / 25.0;
        }
    }
    float sxx = 0.0, sxy = 0.0, syy = 0.0, sxt = 0.0, syt = 0.0, stt = 0.0;
    float sx = 0.0, sy = 0.0, st = 0.0, sa = 0.0, saa = 0.0, n = 0.0;
    [loop] for (int j = 1; j < 6; ++j)
    {
        [loop] for (int i = 1; i < 6; ++i)
        {
            const int   c  = j * 7 + i;
            const float q  = (all_in ? 1.0 : w[c]) * (SGK > 0.0 ? saturate(sr[c] / (SGK * srm + 1e-12)) : 1.0);
            const float gx = 0.25 * ((a[c + 1] - a[c - 1]) + (b[c + 1] - b[c - 1]));   // per cell
            const float gy = 0.25 * ((a[c + 7] - a[c - 7]) + (b[c + 7] - b[c - 7]));
            const float t  = b[c] - a[c];
            sxx += q * gx * gx; sxy += q * gx * gy; syy += q * gy * gy;
            sxt += q * gx * t;  syt += q * gy * t;  stt += q * t * t;
            sx  += q * gx;      sy  += q * gy;      st  += q * t;
            sa  += q * a[c];    saa += q * a[c] * a[c];
            n   += q;
        }
    }
    n = max(n, 1e-3);
    dev  = sqrt(max(saa - sa * sa / n, 0.0) / n);
    // How far the window's content reaches, in cells: its contrast over its mean gradient. A
    // gradient step is good for about that far -- a soft texture many cells across can be
    // followed over a cell or two, lettering two cells across cannot.
    reach = dev / sqrt(max((sxx + syy) / n, 1e-12));
    const float lkc = FlowOffsetShare(sxx, sxy, syy, sxt, syt, stt, sx, sy, st, n, DLSS5_FLOW_FLOOR * DLSS5_FLOW_FLOOR);
    sxx -= lkc * sx * sx / n; sxy -= lkc * sx * sy / n; syy -= lkc * sy * sy / n;
    sxt -= lkc * sx * st / n; syt -= lkc * sy * st / n; stt -= lkc * st * st / n;
    lmin = 0.5 * (sxx + syy - sqrt((sxx - syy) * (sxx - syy) + 4.0 * sxy * sxy)) / n;
    lmax = (sxx + syy) / n - lmin;
    // The step is damped in the direction the window says little about, against what it says in
    // the other: along a floor joint with a faint grain beside it the plain solution divides a
    // small number by a small number and slides the vector up to a cell along the joint, step
    // after step. Where the window cannot tell, the vector stays what it was handed.
#ifndef LKMU
    #define LKMU 0.03
#endif
    const float mu  = n * (LKMU * lmax + DLSS5_FLOW_FLOOR * DLSS5_FLOW_FLOOR);
    const float det = (sxx + mu) * (syy + mu) - sxy * sxy;
    float2 d = 0.0;
    if (det > 1e-16) d = clamp(-float2((syy + mu) * sxt - sxy * syt, (sxx + mu) * syt - sxy * sxt) / det, -1.0, 1.0);   // cells
    res = sqrt(max(stt + d.x * sxt + d.y * syt, 0.0) / n);
    return v + d * cell;
}

// Two gradient steps from v; the result is taken if the window pins motion down in both
// directions, the second step was small (it settled) and what is left is a small part of the
// window's contrast. trust: 0..1 on the same grounds.
float2 FlowRefine(sampler2D sC, sampler2D sP, float2 uv, float2 v, float cell, inout float trust)
{
    float lmin, lmax, res, dev, reach, reach0;
#ifndef LKIT
    #define LKIT 3
#endif
#ifndef LKALL
    #define LKALL 0
#endif
    float2 v1 = FlowTrack(sC, sP, uv, v,  cell, lmin, lmax, res, dev, reach0);
    #if LKIT >= 3
    v1 = FlowTrack(sC, sP, uv, v1, cell, lmin, lmax, res, dev, reach);
    #endif
    #if LKIT >= 4
    v1 = FlowTrack(sC, sP, uv, v1, cell, lmin, lmax, res, dev, reach);
    #endif
    const float2 v2 = FlowTrack(sC, sP, uv, v1, cell, lmin, lmax, res, dev, reach);
#ifndef LKMAX
    #define LKMAX 0.6
#endif
#ifndef LKR
    #define LKR 2.0
#endif
    const float lim = clamp(LKR * min(reach0, reach), LKMAX, 2.5);
    const float  g  = 2.0 * DLSS5_FLOW_FLOOR;
#ifndef LKMAX
    #define LKMAX 0.6
#endif
    // (and it stayed within reach of where it started: the block match is right to half a cell
    // where it is right at all, so a step further than that is the gradients sliding along a
    // line of text or the rows of a list, not a correction)
    #if LKALL == 1
    const bool   ok = lmin >= g * g;
    #elif LKALL == 2
    const bool   ok = lmin >= g * g && all(abs(v2 - v1) <= 0.25 * cell);
    #else
    const bool   ok = lmin >= g * g && all(abs(v2 - v1) <= 0.25 * cell) && all(abs(v2 - v) <= lim * cell) && res <= 0.35 * dev + 2.0 * DLSS5_FLOW_FLOOR;
    #endif
    // The window's weakest direction has the last word on trust either way: a block match on a
    // line is a perfect match, at any point along the line. (P9Corner, which is all the block
    // match has to go by, lets a line through that does not run along a row, a column or a
    // diagonal: the floor joints of the courtyard kept their made-up vectors.)
    const float g1 = 4.0 * DLSS5_FLOW_FLOOR;
    // -- and that direction is weighed against the strong one. A floor joint with a faint grain
    // beside it passes any absolute test in both directions; but what the grain says along the
    // joint is a hundredth of what the joint says across it, and the least error in the joint's
    // own equation (a line is never quite straight, a pyramid never quite alias-free) outweighs
    // it. The courtyard under a strafe: joints holding (-19.6, -0.2) against a true (-8.3, -6.1)
    // -- off by exactly a slide along themselves -- trusted, and handed to the tile beside them.
#ifndef ANI
    #define ANI 0.05
#endif
    const float pin = lmin / (lmin + g1 * g1 + ANI * lmax);
    if (!ok) { trust = min(trust, pin); return v; }
    trust = pin * (1.0 - smoothstep(0.35, 0.7, res / max(dev, 1e-6)));
    return v2;
}

// sC, sP: this level of the pyramid, this frame and the last. sCw, sPw: the next coarser one.
float4 FlowLevel(float2 uv, float lod, sampler2D coarse, bool has_coarse, sampler2D sC, sampler2D sP, sampler2D sCw, sampler2D sPw)
{
    const float  cell = exp2(lod);                 // one texel of this level, in pixels
    const float2 t    = BUFFER_PIXEL_SIZE * cell;
    const P9     c    = Patch9(sC, uv, t, 0.0);
    const bool   wide = lod < 5.5;                 // the pyramid ends at level 6
    P9 cw = c;
    if (wide) cw = Patch9(sCw, uv, t * 2.0, 0.0);
    // What standing still leaves unexplained, sample by sample (see P9CostL).
    P9 m  = Patch9Y(sC, uv, t);
    P9 mw = m;
    if (wide) mw = Patch9Y(sCw, uv, t * 2.0);
    const float changed = m.r1.y > FLOW_NOISE ? 1.0 : 0.0;   // did this cell itself change?
    const float  mc = P9Mean(c);
    const float  contrast = wide ? 0.5 * (P9Contrast(c, mc) + P9Contrast(cw, P9Mean(cw))) : P9Contrast(c, mc);
    const float  corner   = wide ? 0.5 * (P9Corner(c) + P9Corner(cw)) : P9Corner(c);
    float2 ref = 0.0;                              // the coarser level's answer for this spot
    float  ref_trust = 0.0;
    if (has_coarse) { const float4 r = tex2Dlod(coarse, float4(uv, 0.0, 0.0)); ref = r.xy; ref_trust = r.z; }

    const float zero_raw = FlowCost(c, m, cw, mw, uv, float2(0.0, 0.0), t, wide, sP, sPw);
    // A patch with next to nothing in it -- the middle of a plain wall, of a floor, a sky, a soft
    // gradient -- looks the same wherever it goes. It decides nothing: it takes the coarser
    // level's word, and FlowAgree then gives it what the cells around it found. (It used to be
    // let into the contest below when it had changed at all, where every candidate fits it
    // equally and "did not move" has the head start: floors and walls came out standing still
    // under a moving camera. A flat patch that really is still, next to something that moves,
    // inherits that motion; the per-pixel stage sees its pixels did not change and gives zero.)
    // It keeps half of the trust that vector had: a soft texture that only the coarser levels can
    // see (the grain of a floor) was matched there, and matched well -- but to a fraction of a
    // cell twice the size, and "did not move" from a level of 64 px cells says only that the
    // motion was under a few pixels.
    // -- unless the gradients of a wider window can follow it, which they often can.
#ifndef FLATK
    #define FLATK 16.0
#endif
    if (has_coarse && contrast < FLATK * DLSS5_FLOW_FLOOR)
    {
        float tr = 0.5 * ref_trust;
        // "Did not move", handed down to a patch that changed, is not believed at all: a HUD
        // element in a 64 px cell makes the whole cell stand still, and the plain floor beside
        // it used to inherit that -- a ring of dead vectors a hundred pixels wide around every
        // caption and counter while the camera moved.
        if (changed > 0.5 && dot(ref, ref) < 0.01 * cell * cell) tr = 0.0;
        const float2 vr = FlowRefine(sC, sP, uv, ref, cell, tr);
        return float4(vr, tr, changed);
    }
    // The patch is where it was, to within the noise, and there was enough in it to have seen
    // it move. (This is most of a menu, and all of a still picture -- the estimator costs next
    // to nothing there.)
#ifndef CZ
    #define CZ 99.0
#endif
    if (lod < CZ && zero_raw <= DLSS5_FLOW_FLOOR) return float4(0.0, 0.0, FlowTrust(corner, contrast, zero_raw), changed);

    // Zero first, with its head start: everything else has to beat it by a margin.
    const float n0 = length(ref) / cell;
    float2 best      = 0.0;
    float  best_raw  = zero_raw;
#ifndef ZH
    #define ZH 1
#endif
#if ZH == 1
    float  best_cost = zero_raw * (1.0 + 0.04 * n0) + 0.25 * DLSS5_FLOW_FLOOR * n0;
    if (lod < CZ) best_cost = best_cost * (1.0 - ZERO_BIAS) - DLSS5_FLOW_FLOOR;
#elif ZH == 0
    float  best_cost = zero_raw * (1.0 + 0.04 * n0) + 0.25 * DLSS5_FLOW_FLOOR * n0;
#else
    // head start only where the coarser level did not find motion either
    const float hs = 1.0 - saturate(n0 * 2.0 * ref_trust);
    float  best_cost = (zero_raw * (1.0 + 0.04 * n0) + 0.25 * DLSS5_FLOW_FLOOR * n0) * (1.0 - ZERO_BIAS * hs) - DLSS5_FLOW_FLOOR * hs;
#endif

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
    if (lod > 5.5)
    {
        // Coarsest level: whatever was handed down is a frame old, so look two cells in every direction.
        [unroll] for (int y = -2; y <= 2; ++y)
        [unroll] for (int x = -2; x <= 2; ++x)
            if (x != 0 || y != 0)
                DLSS5_FLOW_TRY(float2(x, y) * cell)
    }

    // One cell around the winner...
    const float2 b0 = best;
#ifndef SR
    #define SR 1
#endif
    if (lod > 1.5 && SR > 1)
    {
        [loop] for (int k1 = 0; k1 < (2 * SR + 1) * (2 * SR + 1); ++k1)
        {
            const float2 o1 = float2(k1 % (2 * SR + 1) - SR, k1 / (2 * SR + 1) - SR);
            DLSS5_FLOW_TRY(b0 + o1 * cell)
        }
    }
    else
    {
        [unroll] for (int y1 = -1; y1 <= 1; ++y1)
        [unroll] for (int x1 = -1; x1 <= 1; ++x1)
            if (x1 != 0 || y1 != 0)
                DLSS5_FLOW_TRY(b0 + float2(x1, y1) * cell)
    }

    // ...then half a cell either side on each axis and the fitted minimum.
    const float2 b1 = best;
    const float  h  = 0.5 * cell;
    const float  c0 = FlowSym(c, m, uv, b1, cell, sC, sP);
    const float  cl = FlowSym(c, m, uv, b1 + float2(-h, 0.0), cell, sC, sP);
    const float  cr = FlowSym(c, m, uv, b1 + float2( h, 0.0), cell, sC, sP);
    const float  cu = FlowSym(c, m, uv, b1 + float2(0.0, -h), cell, sC, sP);
    const float  cd = FlowSym(c, m, uv, b1 + float2(0.0,  h), cell, sC, sP);
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
        const float ef = FlowSym(c, m, uv, f, cell, sC, sP);
        if (ef < c0 * (1.0 - ZERO_BIAS) - DLSS5_FLOW_FLOOR) { best = f; best_raw = ef; }
    }
    float trust = FlowTrust(corner, contrast, best_raw);
    best = FlowRefine(sC, sP, uv, best, cell, trust);
    return float4(best, trust, changed);   // xy vector (px), z trust, w 1 if the patch changed
}

// The coarsest level has no coarser one to start from. It starts from LAST FRAME'S field instead
// (motion changes little in a frame): wherever its own picture cannot tell -- along a lone
// floor joint, on a plain wall -- it then keeps what the scene did a frame ago, which the finer
// levels and the cells around had a whole frame to get right, and what is known spreads a
// little further every frame. Started from nothing, "cannot tell" came out as whatever lay
// nearest to no motion at all: beside a diagonal joint the sub-cell fit slid from zero along x
// until the joint's own equation was met, (-19.6, -0.2) for a true (-8.3, -6.1), every finer
// level inherited that and handed it to the tiles on either side.
#ifndef TPRIOR
    #define TPRIOR 1
#endif
float4 PS_Flow6(float4 vpos : SV_Position, float2 uv : TEXCOORD) : SV_Target { return FlowLevel(uv, 6.0, sDLSS5_FlowKeep, TPRIOR, sDLSS5_CurL6, sDLSS5_PrevL6, sDLSS5_CurL6, sDLSS5_PrevL6); }
float4 PS_FlowKeep(float4 vpos : SV_Position, float2 uv : TEXCOORD) : SV_Target { return tex2Dlod(sDLSS5_Flow4L, float4(uv, 0.0, 0.0)); }
float4 PS_Flow5(float4 vpos : SV_Position, float2 uv : TEXCOORD) : SV_Target { return FlowLevel(uv, 5.0, sDLSS5_Flow6, true, sDLSS5_CurL5, sDLSS5_PrevL5, sDLSS5_CurL6, sDLSS5_PrevL6); }
float4 PS_Flow4(float4 vpos : SV_Position, float2 uv : TEXCOORD) : SV_Target { return FlowLevel(uv, 4.0, sDLSS5_Flow5, true, sDLSS5_CurL4, sDLSS5_PrevL4, sDLSS5_CurL5, sDLSS5_PrevL5); }
float4 PS_Flow3(float4 vpos : SV_Position, float2 uv : TEXCOORD) : SV_Target { return FlowLevel(uv, 3.0, sDLSS5_Flow4, true, sDLSS5_CurL3, sDLSS5_PrevL3, sDLSS5_CurL4, sDLSS5_PrevL4); }
float4 PS_Flow2(float4 vpos : SV_Position, float2 uv : TEXCOORD) : SV_Target { return FlowLevel(uv, 2.0, sDLSS5_Flow3, true, sDLSS5_CurL2, sDLSS5_PrevL2, sDLSS5_CurL3, sDLSS5_PrevL3); }
float4 PS_Flow1(float4 vpos : SV_Position, float2 uv : TEXCOORD) : SV_Target { return FlowLevel(uv, 1.0, sDLSS5_Flow2, true, sDLSS5_CurL1, sDLSS5_PrevL1, sDLSS5_CurL2, sDLSS5_PrevL2); }

// Every cell above chose for itself, and in a rendered scene neighbours do not choose alike:
// brickwork, tiles, planks match in more than one place, a plain floor matches everywhere and
// "did not move" has a head start. Seen as a picture (the vector debug view, tools/fx-testbed's
// courtyard under a strafe) the field was a patchwork -- a dot pattern of moving and still
// cells across a brick wall, planks pointing up and down at random, the floor not moving at
// all -- where LumeniteFX, which filters at every level, shows one smooth motion. A temporal
// model fed the patchwork lets its history slide wherever the cells disagree.
// So after every level its cells are made to agree: each takes, of the 5x5 around it, the
// vector that the others -- weighted by how far each can be trusted -- are closest to in sum.
// That is one of the vectors actually found, never an average (so no half-way vector on the
// border between two motions); the odd cell out loses to its neighbours; and a cell with
// nothing to go by, whose own weight is next to nothing, takes what the confident ones say.
// Trust: the cell had something to match (contrast well over the noise) and the match explains
// it (what is left over is a small part of that contrast).
float4 FlowAgree(sampler2D raw, float2 uv, float lod)
{
    const float2 t = BUFFER_PIXEL_SIZE * exp2(lod);
    float2 v[25];
    float  w[25];
    [unroll] for (int k = 0; k < 25; ++k)
    {
        const float4 f = tex2Dlod(raw, float4(uv + float2(k % 5 - 2, k / 5 - 2) * t, 0.0, 0.0));
        v[k] = f.xy;
        w[k] = 0.02 + f.z;
    }
    // A cell that changed and cannot speak for itself does not listen to the ones that stand
    // still (see FlowLevel): the still thing next to it is something else.
    const float4 own = tex2Dlod(raw, float4(uv, 0.0, 0.0));
    if (own.w > 0.5 && own.z < 0.25)
    {
        [unroll] for (int k0 = 0; k0 < 25; ++k0)
            if (k0 != 12 && dot(v[k0], v[k0]) < 1e-4) w[k0] = 0.0;
    }
    float  best = 1e30;
    float2 pick = v[12];
    [loop] for (int i = 0; i < 25; ++i)
    {
        float sum = 0.0;
        [loop] for (int j = 0; j < 25; ++j) sum += w[j] * (abs(v[i].x - v[j].x) + abs(v[i].y - v[j].y));
        // ties (a region that agrees already) go to the cell's own vector
        if (sum < best - 1e-6 || (i == 12 && sum <= best + 1e-6)) { best = sum; pick = v[i]; }
    }
    return float4(pick, own.zw);
}
// A level, agreed, written not as vectors but as what each cell KNOWS about its motion: the
// gradient matrix G of its 5x5 window and the right-hand side r of G v = r, taken around the
// cell's agreed vector v0 (r = G v0 - sum(gradient x what v0 leaves unexplained)).
// A cell with a corner in it has a full-rank G and the equation gives back its own vector, a
// little more exactly. A cell on a line has a G that only speaks for the direction across the
// line -- and says nothing false about the other. A cell on a plain surface has no G at all.
//
// The equations are written for the cell's motion RELATIVE TO THE COARSER LEVEL'S FIELD at the
// cell (the right-hand side has G x that field taken off): what FlowRelax then smooths is a
// correction to that field, not a motion. That matters wherever the motion itself varies from
// cell to cell, which under a camera is everywhere but straight ahead -- a floor under a pan
// runs from 55 px a frame at the edge of the screen to 28 in the middle, under a dolly from
// -39 to +36 -- and a smoothness that pulled neighbours towards the SAME motion would flatten
// exactly that.
void FlowEq(sampler2D raw, sampler2D sC, sampler2D sP, sampler2D sS, bool has_prior, float2 uv, float lod, out float4 A, out float2 B, out float4 St)
{
    const float  cell = exp2(lod);
    const float2 px = BUFFER_PIXEL_SIZE;
    const float4 ag = FlowAgree(raw, uv, lod);
    const float2 v0 = ag.xy;
    const float2 vi = round(v0 / cell) * cell, vf = v0 - vi;
    float a[49], b[49], w[49], dd[49];
    [loop] for (int k = 0; k < 49; ++k)
    {
        const float2 o  = float2(k % 7 - 3, k / 7 - 3) * cell;
        const float2 ca = tex2Dlod(sC, float4(uv + (o - 0.5 * vf) * px, 0.0, 0.0)).xy;
        a[k] = 64.0 * ca.x; dd[k] = 64.0 * ca.y; w[k] = abs(ca.y);
        b[k] = 64.0 * tex2Dlod(sP, float4(uv + (o + vi + 0.5 * vf) * px, 0.0, 0.0)).x;
    }
    // (as in FlowTrack: a sample that standing still explains better has no say)
    float cnt = 0.0;
    [loop] for (int k9 = 0; k9 < 49; ++k9)
    {
        w[k9] = abs(b[k9] - a[k9]) <= 64.0 * (w[k9] + FLOW_NOISE) ? 1.0 : 0.0;
        cnt += w[k9];
    }
    const bool all_in = cnt < 10.0;
    // (see FlowStillRatio)
    float sr[49];
    float srm = 0.0;
    [loop] for (int j0 = 1; j0 < 6; ++j0)
    {
        [loop] for (int i0 = 1; i0 < 6; ++i0)
        {
            const int    c0 = j0 * 7 + i0;
            const float2 g0 = 0.5 * float2(a[c0 + 1] - a[c0 - 1], a[c0 + 7] - a[c0 - 7]);
            const float2 d0 = 0.5 * float2(dd[c0 + 1] - dd[c0 - 1], dd[c0 + 7] - dd[c0 - 7]);
            sr[c0] = FlowStillRatio(dot(g0, g0), dot(d0, d0), 4.0 * DLSS5_FLOW_FLOOR * DLSS5_FLOW_FLOOR * 4096.0);
            srm += sr[c0] / 25.0;
        }
    }
    float sxx = 0.0, sxy = 0.0, syy = 0.0, sxt = 0.0, syt = 0.0, stt = 0.0;
    float sx = 0.0, sy = 0.0, st = 0.0, sa = 0.0, saa = 0.0, n = 0.0;
    [loop] for (int j = 1; j < 6; ++j)
    {
        [loop] for (int i = 1; i < 6; ++i)
        {
            const int   c  = j * 7 + i;
            const float wq = (all_in ? 1.0 : w[c]) * (SGK > 0.0 ? saturate(sr[c] / (SGK * srm + 1e-12)) : 1.0);
            const float gx = 0.25 * ((a[c + 1] - a[c - 1]) + (b[c + 1] - b[c - 1]));
            const float gy = 0.25 * ((a[c + 7] - a[c - 7]) + (b[c + 7] - b[c - 7]));
            const float t  = b[c] - a[c];
            sxx += wq * gx * gx; sxy += wq * gx * gy; syy += wq * gy * gy;
            sxt += wq * gx * t;  syt += wq * gy * t;  stt += wq * t * t;
            sx  += wq * gx;      sy  += wq * gy;      st  += wq * t;
            sa  += wq * a[c];    saa += wq * a[c] * a[c];
            n   += wq;
        }
    }
    n = max(n, 1e-3);
    const float lkc = FlowOffsetShare(sxx, sxy, syy, sxt, syt, stt, sx, sy, st, n, DLSS5_FLOW_FLOOR * DLSS5_FLOW_FLOOR * 4096.0);
    sxx = (sxx - lkc * sx * sx / n) / n; sxy = (sxy - lkc * sx * sy / n) / n; syy = (syy - lkc * sy * sy / n) / n;
    sxt = (sxt - lkc * sx * st / n) / n; syt = (syt - lkc * sy * st / n) / n; stt = (stt - lkc * st * st / n) / n;
    // A window that the agreed vector does not explain -- an outline with something else behind
    // it, a disocclusion -- has no say: what is left over, against the window's own contrast.
    const float dev = sqrt(max(saa - sa * sa / n, 0.0) / n);
    // (what is left over AFTER the step these equations ask for: a soft texture a cell away from
    // where the vector put it is not "unexplained", it is exactly what the equations are for)
#ifndef QPOST
    #define QPOST 1
#endif
    const float e9  = 1e-7 + 0.01 * (sxx + syy);
    const float expl = ((syy + e9) * sxt * sxt - 2.0 * sxy * sxt * syt + (sxx + e9) * syt * syt) / ((sxx + e9) * (syy + e9) - sxy * sxy);
    const float q   = 1.0 - smoothstep(0.35, 0.7, sqrt(max(stt - (QPOST ? expl : 0.0), 0.0)) / max(dev, 1e-4));
    float2 prior = 0.0;
    if (has_prior) prior = tex2Dlod(sS, float4(uv, 0.0, 0.0)).xy;
    const float2 v0c = (v0 - prior) / cell;
    const float2 r   = float2(sxx * v0c.x + sxy * v0c.y - sxt, sxy * v0c.x + syy * v0c.y - syt) * q;
    // The cell's own vector, as far as it can be trusted, is evidence too -- in every direction,
    // which the gradients of a line are not: without it a floor joint, cut off from the floor
    // around it by its own darkness, has nothing to say along its length and the coarsest level
    // has nothing to start it from.
#ifndef KAPK
    #define KAPK 3.0
#endif
    const float rt  = sqrt((sxx - syy) * (sxx - syy) + 4.0 * sxy * sxy);
    const float l1  = 0.5 * (sxx + syy - rt), l2 = 0.5 * (sxx + syy + rt);
    const float rnd = l1 / (l1 + ANI * l2 + 1e-12);
    const float k = KAPK * (16.0 * DLSS5_FLOW_FLOOR * DLSS5_FLOW_FLOOR * 4096.0 + (sxx + syy) * q) * ag.z * ag.z * rnd * rnd;
#ifndef EQK
    #define EQK 1.0
#endif
    A  = float4(EQK * sxx * q + k, EQK * sxy * q, EQK * syy * q + k, EQK * r.x + k * v0c.x);
    B  = float2(EQK * r.y + k * v0c.y, 0.0);
    // "Stands still": found still and did not change. (A translucent panel's inside is found
    // still by some cells and changes with the picture under it: that is not this.)
    St = float4(0.0, 0.0, tex2Dlod(sC, float4(uv, 0.0, 0.0)).x, dot(v0, v0) < 1e-4 * cell * cell && ag.w < 0.5 ? 1.0 : 0.0);
}

// The level's field: every cell's own equations, and agreement with the cells around it that
// look like it. One relaxation step of
//     sum over cells of  (d - d_own)' G (d - d_own)  +  lambda x sum over neighbours of w |d - d_n|^2
// in the correction d to the coarser level's field: a cell whose window pins its motion down
// (G large against lambda) keeps what it measured; a line keeps what it measured ACROSS itself
// and takes the rest from around it; a plain surface takes all of it from around it -- from
// the cells of its own brightness first (w), so the floor seen between the bars of a railing
// moves with the floor and not with the railing. Run a few times at different reaches.
// (Until this the equations were pooled over squares of 2, 4, 8... cells and solved there, the
// squares blended by how much each knew. The squares showed: the field in a plain area was a
// patchwork of them, several pixels apart at the seams, where LumeniteFX -- which filters --
// has one smooth motion. tools/fx-testbed, the courtyard's floor under a dolly: 3.7 px.)
// A cell whose picture changed does not listen to cells that stand still: what stands still
// next to something moving is something else -- a HUD over the game, a caption.
#ifndef LAMK
    #define LAMK 4.0
#endif
#ifndef LUMK
    #define LUMK 10.0
#endif
#ifndef WMIN
    #define WMIN 0.1
#endif
float4 FlowRelax(sampler2D eqA, sampler2D eqB, sampler2D st, sampler2D raw, float2 uv, float lod, float reach)
{
    const float2 t = BUFFER_PIXEL_SIZE * exp2(lod) * reach;
    const float4 A = tex2Dlod(eqA, float4(uv, 0.0, 0.0));
    const float  ry = tex2Dlod(eqB, float4(uv, 0.0, 0.0)).x;
    const float4 c = tex2Dlod(st, float4(uv, 0.0, 0.0));
    const bool   changed = tex2Dlod(raw, float4(uv, 0.0, 0.0)).w > 0.5;
    float  W = 0.0;
    float2 sum = 0.0;
#ifndef CM0
    #define CM0 0.35
#endif
#ifndef CMK
    #define CMK 1.0
#endif
    const float tr0 = 16.0 * DLSS5_FLOW_FLOOR * DLSS5_FLOW_FLOOR * 4096.0;
    [unroll] for (int k = 0; k < 9; ++k)
    {
        if (k == 4) continue;
        const float4 n = tex2Dlod(st, float4(uv + float2(k % 3 - 1, k / 3 - 1) * t, 0.0, 0.0));
        float w = max(saturate(1.0 - LUMK * abs(n.z - c.z)), WMIN);
        // ...or it moves the way this cell measured, as far as this cell measured anything: a
        // dark floor joint is not the floor's brightness, but the floor's motion satisfies the
        // joint's equation (which only speaks across the joint) -- so the joint takes the rest
        // from the floor. The bar of a railing in front of that floor does not: the floor's
        // motion contradicts what the bar measured.
        const float2 rho = float2(A.x * n.x + A.y * n.y - A.w, A.y * n.x + A.z * n.y - ry);
        w = max(w, CMK * saturate((A.x + A.z) / tr0) * saturate(1.0 - length(rho) / ((A.x + A.z + 1e-12) * CM0)));
        if (changed && n.w > 0.5) w = 0.0;
        W += w; sum += w * n.xy;
    }
    // (lambda: a cell that pins motion down as well as the estimator asks of a trusted one --
    // 4 x its floor, squared, in the 64x units of the sums -- weighs as much as eight neighbours)
    const float lam = LAMK * 2.0 * DLSS5_FLOW_FLOOR * DLSS5_FLOW_FLOOR * 4096.0;
    const float m11 = A.x + lam * W, m12 = A.y, m22 = A.z + lam * W;
    const float2 rhs = float2(A.w, ry) + lam * sum;
    const float det = m11 * m22 - m12 * m12;
    float2 d = c.xy;
    if (det > 1e-20) d = clamp(float2(m22 * rhs.x - m12 * rhs.y, m11 * rhs.y - m12 * rhs.x) / det, -4.0, 4.0);
    return float4(d, c.zw);
}
float4 FlowFinish(float4 st, sampler2D raw, sampler2D sS, bool has_prior, float2 uv, float lod)
{
    float2 v = st.xy * exp2(lod);
    if (has_prior) v += tex2Dlod(sS, float4(uv, 0.0, 0.0)).xy;
    return float4(v, tex2Dlod(raw, float4(uv, 0.0, 0.0)).zw);
}
#ifndef RS1
    #define RS1 3.0
#endif
#ifndef RS2
    #define RS2 2.0
#endif
#ifndef RS3
    #define RS3 1.0
#endif
#ifndef RS4
    #define RS4 1.0
#endif
#define DLSS5_FLOW_LEVEL(N, LOD, SPRIOR, HAS) \
    void   PS_FlowEq##N(float4 vpos : SV_Position, float2 uv : TEXCOORD, out float4 A : SV_Target0, out float2 B : SV_Target1, out float4 St : SV_Target2) { FlowEq(sDLSS5_Flow##N##Raw, sDLSS5_CurL##N, sDLSS5_PrevL##N, SPRIOR, HAS, uv, LOD, A, B, St); } \
    float4 PS_FlowR1##N(float4 vpos : SV_Position, float2 uv : TEXCOORD) : SV_Target { return FlowRelax(sDLSS5_FlowEqA##N, sDLSS5_FlowEqB##N, sDLSS5_FlowStA##N, sDLSS5_Flow##N##Raw, uv, LOD, RS1); } \
    float4 PS_FlowR2##N(float4 vpos : SV_Position, float2 uv : TEXCOORD) : SV_Target { return FlowRelax(sDLSS5_FlowEqA##N, sDLSS5_FlowEqB##N, sDLSS5_FlowStB##N, sDLSS5_Flow##N##Raw, uv, LOD, RS2); } \
    float4 PS_FlowR3##N(float4 vpos : SV_Position, float2 uv : TEXCOORD) : SV_Target { return FlowRelax(sDLSS5_FlowEqA##N, sDLSS5_FlowEqB##N, sDLSS5_FlowStA##N, sDLSS5_Flow##N##Raw, uv, LOD, RS3); } \
    float4 PS_FlowFill##N(float4 vpos : SV_Position, float2 uv : TEXCOORD) : SV_Target { return FlowFinish(FlowRelax(sDLSS5_FlowEqA##N, sDLSS5_FlowEqB##N, sDLSS5_FlowStB##N, sDLSS5_Flow##N##Raw, uv, LOD, RS4), sDLSS5_Flow##N##Raw, SPRIOR, HAS, uv, LOD); }
DLSS5_FLOW_LEVEL(6, 6.0, sDLSS5_FlowKeep, TPRIOR)   // last frame's field: see PS_Flow6
DLSS5_FLOW_LEVEL(5, 5.0, sDLSS5_Flow6L, true)
DLSS5_FLOW_LEVEL(4, 4.0, sDLSS5_Flow5L, true)
DLSS5_FLOW_LEVEL(3, 3.0, sDLSS5_Flow4L, true)
DLSS5_FLOW_LEVEL(2, 2.0, sDLSS5_Flow3L, true)
DLSS5_FLOW_LEVEL(1, 1.0, sDLSS5_Flow2L, true)

// ---------------------------------------------------------------------------------------------
// The base field: the four coarse levels (cells of 64, 32, 16 and 8 pixels).
//
// They used to be built like the fine ones -- a 3x3 patch of texels as large as the cell, a
// search over whole cells, a head start for "did not move", trust by contrast -- and on
// tools/fx-testbed's 3D scene, with and without a depth buffer, that was the weak part: soft
// surfaces under a travelling camera a pixel out, a lone floor joint dragging the floor
// beside it along itself. Each of those two failures names its own cure -- a patch as large
// as its cell reaches into the next thing over, and a search that takes any improvement at
// all slides along a line -- and the base is now built on them:
//   - a small patch (thirteen points: a cross reaching three texels, and the four diagonals)
//     on texels FOUR TIMES FINER than the cell. What a level can see is then not tied to how
//     coarsely it places its vectors, and the patch stays inside its own cell: it does not
//     reach across to a HUD panel or to the next bar of a railing;
//   - no search to speak of. A cell tries what the coarser level found here and around here,
//     what the scene did a frame ago, and "did not move"; then walks downhill a texel at a
//     time, a few steps at most; then a parabola through the last three costs on each axis,
//     kept only if the cost measured there really is lower. Nothing can jump, and where the
//     picture cannot tell two candidates apart the first one stands -- which is the coarser
//     level's, i.e. the smooth one;
//   - every cell decides on what it sees, however little that is (no "too flat to say"),
//     and then the cells VOTE, one cell one vote: of the 3x3 around it each takes the vector
//     the others are closest to. A joint one cell wide is outvoted by the floor on both sides
//     of it, instead of the floor being told by the joint because the joint has more contrast.
// The fine levels (4 and 2 pixel cells) are as they were, started from this.
// ---------------------------------------------------------------------------------------------
#ifndef BASE_STEPS
    #define BASE_STEPS 4
#endif
#ifndef BASE_ZK
    #define BASE_ZK 0.5
#endif
#ifndef BASE_FITS
    #define BASE_FITS 2
#endif
// BASE_VFIT: a V through the three costs instead of a parabola (the textbook cure for the
// way a parabola on absolute differences pulls every fraction towards the whole texel).
// Measured: a slow pan gains (0.30 -> 0.25 px) and things that move by themselves lose more
// (0.41 -> 0.66: along the stripes of a rolling ball a V has no slope to stand on). Off.
#ifndef BASE_VFIT
    #define BASE_VFIT 0
#endif
// BASE_CAM: the camera fit inside the estimator (see BaseLevel). 1 = where a cell starts
// from; 2 = and what it ends on wherever that fits about as well. OFF BY DEFAULT, for the same
// reason as CAMERA_MODEL: on tools/fx-testbed it makes the vectors more exact (under a pan
// 0.93 -> 0.78 px on textured surfaces, 4.0 -> 3.0 at outlines; with depth it also helps a
// strafe) and the DLAA picture no better -- 0.57 from the input against 0.51 under the pan,
// the turning point of the pan 1.37 times as restless as the input against 1.32. Something
// that consumes the vectors AS vectors (frame generation, motion blur) would be better off
// with it on. It costs the fit: about half a millisecond.
#ifndef BASE_CAM
    #define BASE_CAM 0
#endif
#ifndef BASE_CAM_K
    #define BASE_CAM_K 1.1
#endif
#ifndef BASE_CAM_F
    #define BASE_CAM_F 0.5
#endif
#ifndef BASE_CAM_CELL
    #define BASE_CAM_CELL 64.0
#endif
#ifndef BASE_FITM
    #define BASE_FITM 1.0
#endif
#ifndef BASE_FITF
    #define BASE_FITF 0.0
#endif
// Two more uses of depth inside the estimator, measured and left off: weighing a patch's points
// by whether they are on the middle's surface (BASE_DW) and letting only cells of the same
// surface vote (BASE_DV). On tools/fx-testbed the first is a wash (better at the outlines under
// a dolly and a strafe, worse under a pan, a hair worse on plain surfaces) and the second costs
// accuracy on plain surfaces and at outlines alike: the picture-only base is good enough there
// that taking evidence away from it does more harm than taking the wrong evidence does.
#ifndef BASE_DW
    #define BASE_DW 0
#endif
#ifndef BASE_DV
    #define BASE_DV 0
#endif
#ifndef BASE_DEPTH_W
    #define BASE_DEPTH_W 0.02
#endif
#ifndef BASE_DPEN
    #define BASE_DPEN 0.25
#endif
static const float2 kBaseTap[13] = {
    float2(0, 0), float2(-1, 0), float2(1, 0), float2(0, -1), float2(0, 1), float2(-3, 0), float2(3, 0), float2(0, -3), float2(0, 3),
    float2(-2, -2), float2(2, -2), float2(-2, 2), float2(2, 2) };
// With a depth buffer (see DEPTH_USE) a candidate that would take the patch's middle to a
// different surface a frame ago pays for it (dc: the middle's depth now; a step in depth, not
// a slope -- a floor coming closer is the same floor). w weighs the patch's points; it is 1
// everywhere unless BASE_DW is on (see there).
float BaseCost(sampler2D sM, sampler2D sMp, float2 uv, float2 v, float2 f, float tex, float w[13], float dc, bool dz)
{
    // f: the last fraction of the vector, split between the two frames (this one read half of
    // it back, the last one half of it on) so that both are interpolated alike; 0 = read as is.
    const float2 t = BUFFER_PIXEL_SIZE * tex, pa = -0.5 * f * BUFFER_PIXEL_SIZE, pb = (v + 0.5 * f) * BUFFER_PIXEL_SIZE;
    float a[13], b[13];
    float ma = 0.0, mb = 0.0, ws = 0.0;
    [unroll] for (int i = 0; i < 13; ++i)
    {
        a[i] = tex2Dlod(sM,  float4(uv + kBaseTap[i] * t + pa, 0.0, 0.0)).x;
        b[i] = tex2Dlod(sMp, float4(uv + kBaseTap[i] * t + pb, 0.0, 0.0)).x;
        ma += w[i] * a[i]; mb += w[i] * b[i]; ws += w[i];
    }
    ma /= ws; mb /= ws;
    float e = 0.0;
    [unroll] for (int j = 0; j < 13; ++j) e += w[j] * abs((a[j] - ma) - (b[j] - mb));
    e = e / ws + BASE_DPEN * abs(ma - mb);
    if (dz) e += BASE_DEPTH_W * (1.0 - SameSurface(dc, tex2Dlod(sDLSS5_PrevDepth, float4(uv + pb, 0.0, 0.0)).x));
    return e;
}
float BaseMad(sampler2D sM, float2 uv, float tex)
{
    const float2 t = BUFFER_PIXEL_SIZE * tex;
    const float c = tex2Dlod(sM, float4(uv, 0.0, 0.0)).x;
    return 0.125 * (abs(tex2Dlod(sM, float4(uv + float2(-3, 0) * t, 0.0, 0.0)).x - c) + abs(tex2Dlod(sM, float4(uv + float2(3, 0) * t, 0.0, 0.0)).x - c)
                  + abs(tex2Dlod(sM, float4(uv + float2(0, -3) * t, 0.0, 0.0)).x - c) + abs(tex2Dlod(sM, float4(uv + float2(0, 3) * t, 0.0, 0.0)).x - c)
                  + abs(tex2Dlod(sM, float4(uv + float2(-2, -2) * t, 0.0, 0.0)).x - c) + abs(tex2Dlod(sM, float4(uv + float2(2, -2) * t, 0.0, 0.0)).x - c)
                  + abs(tex2Dlod(sM, float4(uv + float2(-2, 2) * t, 0.0, 0.0)).x - c) + abs(tex2Dlod(sM, float4(uv + float2(2, 2) * t, 0.0, 0.0)).x - c));
}
#define DLSS5_BASE_TRY(V) { const float2 v_ = (V); const float e_ = BaseCost(sM, sMp, uv, v_, 0.0, tex, w, dc, dz); if (e_ < best_e) { best_e = e_; best = v_; } }
// A step of the search or of the walk downhill has to be worth taking: a tenth better, and by
// more than rounding. Along a pattern that looks the same a texel further on -- planks, a
// joint, stripes -- every step is a hair better or worse than the last by accident, and a walk
// that took every hair slid thirty pixels along the planks of fx-testbed's turning crate.
#ifndef BASE_MARGIN
    #define BASE_MARGIN 0.9
#endif
#define DLSS5_BASE_STEP(V) { const float2 v_ = (V); const float e_ = BaseCost(sM, sMp, uv, v_, 0.0, tex, w, dc, dz); if (e_ < best_e * BASE_MARGIN - 0.1 * DLSS5_FLOW_FLOOR) { best_e = e_; best = v_; } }
// cell: this level's cell in pixels; tex: the texel matched on (a quarter of it); coarse: the
// level above, read bilinearly (at the top: last frame's field).
float4 BaseLevel(float2 uv, float cell, sampler2D sM, sampler2D sMp, float tex, sampler2D coarse, bool top)
{
    const float2 px = BUFFER_PIXEL_SIZE;
    const float2 cc = px * cell * (top ? 1.0 : 2.0);
    const bool   dz = DepthUsable();
    float w[13];
    float dc = 0.0;
    [unroll] for (int q = 0; q < 13; ++q) w[q] = 1.0;
    if (dz) dc = ReShade::GetLinearizedDepth(uv);
    if (dz && BASE_DW)
    {
        float dt[13];
        [unroll] for (int q1 = 1; q1 < 13; ++q1) dt[q1] = ReShade::GetLinearizedDepth(uv + kBaseTap[q1] * px * tex);
        // (the points come in opposite pairs: 1-2, 3-4, 5-6, 7-8, 9-12, 10-11)
        w[1] = OnSurface(dc, dt[1], dt[2]);   w[2]  = OnSurface(dc, dt[2], dt[1]);
        w[3] = OnSurface(dc, dt[3], dt[4]);   w[4]  = OnSurface(dc, dt[4], dt[3]);
        w[5] = OnSurface(dc, dt[5], dt[6]);   w[6]  = OnSurface(dc, dt[6], dt[5]);
        w[7] = OnSurface(dc, dt[7], dt[8]);   w[8]  = OnSurface(dc, dt[8], dt[7]);
        w[9] = OnSurface(dc, dt[9], dt[12]);  w[12] = OnSurface(dc, dt[12], dt[9]);
        w[10] = OnSurface(dc, dt[10], dt[11]); w[11] = OnSurface(dc, dt[11], dt[10]);
    }
    float2 best   = top ? float2(0.0, 0.0) : tex2Dlod(coarse, float4(uv, 0.0, 0.0)).xy;
    float  best_e = BaseCost(sM, sMp, uv, best, 0.0, tex, w, dc, dz);
#if BASE_CAM
    // What the camera did a frame ago, at this cell's depth (the fit of last frame's field: this
    // frame's does not exist yet, and a camera does not change its mind in a frame -- what it
    // changed by, the walk below makes up). Where that explains the cell about as well as what
    // the level above found, it is where the cell starts from: of two answers the picture cannot
    // tell apart -- this bar of a railing or the next one, anywhere along a plain edge -- the
    // camera's is the right one far more often than not, and it knows about depth outlines,
    // which a level of cells does not. A cell of something that moves by itself is not
    // explained by it and starts where it always did.
    float2 cv = 0.0;
    float  ce = 1e9;
    if (!top && cell <= BASE_CAM_CELL && FitIsUsable())
    {
        cv = PredictMV(uv, 0.0) * BUFFER_SCREEN_SIZE;
        ce = BaseCost(sM, sMp, uv, cv, 0.0, tex, w, dc, dz);
        if (ce <= best_e * BASE_CAM_K + BASE_CAM_F * DLSS5_FLOW_FLOOR) { best = cv; best_e = min(ce, best_e); }
    }
#endif
    if (top)
    {
        // Nothing above: none, or last frame's vector if that fits better, and from there three
        // texels in every direction. (None first: on a sky or a blank wall everything fits
        // alike, and what stood there was whatever vector had last been true of something else
        // -- kept for good, since nothing could ever beat it.)
        DLSS5_BASE_TRY(tex2Dlod(coarse, float4(uv, 0.0, 0.0)).xy)
        const float2 seed = best;
        [loop] for (int k = 0; k < 49; ++k)
        {
            const float2 o = float2(k % 7 - 3, k / 7 - 3);
            if (o.x != 0.0 || o.y != 0.0) DLSS5_BASE_STEP(seed + o * tex)
        }
    }
    else
    {
        [unroll] for (int y = -1; y <= 1; ++y)
        [unroll] for (int x = -1; x <= 1; ++x)
            if (x != 0 || y != 0) DLSS5_BASE_TRY(tex2Dlod(coarse, float4(uv + float2(x, y) * cc, 0.0, 0.0)).xy)
        DLSS5_BASE_TRY(tex2Dlod(sDLSS5_FlowKeep, float4(uv, 0.0, 0.0)).xy)
        // Downhill, a texel at a time.
        [loop] for (int it = 0; it < BASE_STEPS; ++it)
        {
            const float2 c0 = best;
            [loop] for (int k = 0; k < 9; ++k)
                if (k != 4) DLSS5_BASE_STEP(c0 + float2(k % 3 - 1, k / 3 - 1) * tex)
            if (all(best == c0)) break;
        }
    }
    // The last fraction of a texel: half a texel either side on each axis, read symmetrically
    // (see BaseCost), the minimum of the curve through the three costs, kept if the cost
    // measured there really is lower. Twice: the second time from where the first one ended.
    // (Read from one frame only, the interpolated side is the softer of the two and every
    // fractional vector looks a little worse than the whole one next to it.)
    [unroll] for (int fit = 0; fit < BASE_FITS; ++fit)
    {
        const float h  = (fit < 2 ? 0.5 : 0.25) * tex;
        const float el = BaseCost(sM, sMp, uv, best, float2(-h, 0.0), tex, w, dc, dz), er = BaseCost(sM, sMp, uv, best, float2(h, 0.0), tex, w, dc, dz);
        const float eu = BaseCost(sM, sMp, uv, best, float2(0.0, -h), tex, w, dc, dz), ed = BaseCost(sM, sMp, uv, best, float2(0.0, h), tex, w, dc, dz);
#if BASE_VFIT
        // A sum of absolute differences falls away from its minimum in two straight lines, not
        // in a parabola: the minimum is where the two lines of equal and opposite slope
        // through the three costs meet.
        const float ax = max(el, er) - best_e, ay = max(eu, ed) - best_e;
#else
        const float ax = el - 2.0 * best_e + er, ay = eu - 2.0 * best_e + ed;
#endif
        const float2 f = h * float2(ax > 1e-9 ? clamp(0.5 * (el - er) / ax, -1.0, 1.0) : 0.0, ay > 1e-9 ? clamp(0.5 * (eu - ed) / ay, -1.0, 1.0) : 0.0);
        const float ef = BaseCost(sM, sMp, uv, best, f, tex, w, dc, dz);
        if (ef < best_e * BASE_FITM - BASE_FITF * DLSS5_FLOW_FLOOR) { best += f; best_e = ef; }
    }
    // "Did not move" is not one more candidate here: on anything that slides along its own
    // pattern (a striped ball rolling sideways) and under any motion of less than a texel it
    // fits as well as the truth, and as a candidate it won those ties -- a fifth of a slowly
    // panning frame standing still. It is taken only where it fits clearly BETTER than the best
    // moving answer: a still element over a moving picture, which is what it is for. (The
    // per-pixel stage decides the close calls.)
    if (!top && dot(best, best) > 0.0)
    {
        const float e0 = BaseCost(sM, sMp, uv, float2(0.0, 0.0), 0.0, tex, w, dc, dz);
        if (e0 < BASE_ZK * best_e - 0.25 * DLSS5_FLOW_FLOOR) { best = 0.0; best_e = e0; }
    }
#if BASE_CAM >= 2
    // And last: whatever the search ended on, if the camera's answer explains the cell about as
    // well, it IS the answer. The candidates above are taken on any improvement at all, and
    // where the picture has nothing to say (the far end of a floor in haze, a sky, the inside
    // of a plain wall) "any improvement" is noise -- such a cell ended on whichever candidate
    // the noise liked, mostly "did not move". By construction this changes nothing the picture
    // can tell: only cells where the two answers fit alike.
    if (ce <= best_e * BASE_CAM_K + BASE_CAM_F * DLSS5_FLOW_FLOOR) { best = cv; best_e = ce; }
#endif
    const float mad = BaseMad(sM, uv, tex);
    return float4(best, 1.0 - smoothstep(0.3, 0.7, best_e / (mad + DLSS5_FLOW_FLOOR)), 0.0);
}
// One cell, one vote: of the 3x3 around the cell, the vector the others are closest to.
// (BASE_DV: only the cells of this cell's own surface vote. Off: see there.)
float4 BaseAgree(sampler2D raw, float2 uv, float cell)
{
    const float2 t = BUFFER_PIXEL_SIZE * cell;
    const bool   dz = DepthUsable();
    const float  dc = dz ? ReShade::GetLinearizedDepth(uv) : 0.0;
    float2 v[9];
    float  w[9], dn[9];
    [unroll] for (int k = 0; k < 9; ++k)
    {
        const float2 at = uv + float2(k % 3 - 1, k / 3 - 1) * t;
        v[k]  = tex2Dlod(raw, float4(at, 0.0, 0.0)).xy;
        dn[k] = dz ? ReShade::GetLinearizedDepth(at) : 0.0;
    }
    [unroll] for (int k1 = 0; k1 < 9; ++k1) w[k1] = dz && BASE_DV && k1 != 4 ? OnSurface(dc, dn[k1], dn[8 - k1]) : 1.0;
    float  best = 1e30;
    float2 pick = v[4];
    [unroll] for (int i = 0; i < 9; ++i)
    {
        float sum = w[i] < 0.5 ? 1e30 : 0.0;
        [unroll] for (int j = 0; j < 9; ++j) sum += w[j] * (abs(v[i].x - v[j].x) + abs(v[i].y - v[j].y));
        if (sum < best - 1e-6 || (i == 4 && sum <= best + 1e-6)) { best = sum; pick = v[i]; }
    }
    return float4(pick, tex2Dlod(raw, float4(uv, 0.0, 0.0)).zw);
}
float4 PS_BaseMatch6(float4 vpos : SV_Position, float2 uv : TEXCOORD) : SV_Target { return BaseLevel(uv, 64.0, sDLSS5_CurL4, sDLSS5_PrevL4, 16.0, sDLSS5_FlowKeep, true); }
float4 PS_BaseMatch5(float4 vpos : SV_Position, float2 uv : TEXCOORD) : SV_Target { return BaseLevel(uv, 32.0, sDLSS5_CurL3, sDLSS5_PrevL3, 8.0, sDLSS5_Flow6L, false); }
float4 PS_BaseMatch4(float4 vpos : SV_Position, float2 uv : TEXCOORD) : SV_Target { return BaseLevel(uv, 16.0, sDLSS5_CurL2, sDLSS5_PrevL2, 4.0, sDLSS5_Flow5L, false); }
float4 PS_BaseMatch3(float4 vpos : SV_Position, float2 uv : TEXCOORD) : SV_Target { return BaseLevel(uv, 8.0,  sDLSS5_CurL1, sDLSS5_PrevL1, 2.0, sDLSS5_Flow4L, false); }
float4 PS_BaseMatch2(float4 vpos : SV_Position, float2 uv : TEXCOORD) : SV_Target { return BaseLevel(uv, 4.0,  sDLSS5_CurLuma, sDLSS5_PrevLuma, 1.0, sDLSS5_Flow3L, false); }
float4 PS_BaseMatch1(float4 vpos : SV_Position, float2 uv : TEXCOORD) : SV_Target { return BaseLevel(uv, 2.0,  sDLSS5_CurLuma, sDLSS5_PrevLuma, 1.0, sDLSS5_Flow2L, false); }
float4 PS_BaseAgree2(float4 vpos : SV_Position, float2 uv : TEXCOORD) : SV_Target { return BaseAgree(sDLSS5_Flow2Raw, uv, 4.0); }
float4 PS_BaseAgree1(float4 vpos : SV_Position, float2 uv : TEXCOORD) : SV_Target { return BaseAgree(sDLSS5_Flow1Raw, uv, 2.0); }
// The finish. A cell's vector comes from thirteen points, and the last fraction of a pixel of
// it is as noisy as thirteen points make it: under a slow pan, where a fraction of a pixel is
// all there is, neighbouring cells of one wall disagree by a quarter of a pixel. The vote above
// does not help there -- it picks one of the cells, noise and all. So, last of all, each cell
// takes the mean of the cells around it that say THE SAME THING it does (within half a pixel
// and a twentieth of its length): the noise averages out, and a neighbour that moves
// differently -- the other side of an outline -- is not one of them, so nothing is blurred
// across. A cell that stands still stays exactly still, and still cells have no say in a
// moving one.
// With a depth buffer (see DEPTH_USE) the cells also have to be on the cell's own surface, and
// one thing more is possible: a cell that nothing explains (it was hidden a frame ago, or sits
// astride an outline) takes what the explained cells of its surface say instead of its own
// guess -- the wall that comes out from behind a moving thing moves with the wall.
// BASE_FIN: how many times (0 = off). BASE_FIN_R: spacing of the 5x5, in cells.
// MEASURED AND LEFT OFF. The vectors gain a few percent on every shot of tools/fx-testbed
// (textured error under a pan 0.81 -> 0.77 px) and the DLAA picture loses: as far from the
// input again as with the camera fit (0.61 against 0.51 under the pan), and no steadier. What
// a temporal pass wants of a cell is the vector that lines its history up with the picture
// THERE, and that is what the match found -- the mean of its neighbours is nearer the true
// motion and further from that.
#ifndef BASE_FIN
    #define BASE_FIN 0
#endif
#ifndef BASE_FIN_R
    #define BASE_FIN_R 1.0
#endif
#ifndef BASE_FIN_T
    #define BASE_FIN_T 0.5
#endif
#ifndef BASE_FIN_FILL
    #define BASE_FIN_FILL 1
#endif
float4 BaseFin(sampler2D src, float2 uv, float cell)
{
    const float2 t  = BUFFER_PIXEL_SIZE * cell * BASE_FIN_R;
    const bool   dz = DepthUsable();
    const float4 c  = tex2Dlod(src, float4(uv, 0.0, 0.0));
    if (dot(c.xy, c.xy) == 0.0) return c;
    const float  tol = BASE_FIN_T + 0.05 * length(c.xy);
    float  d[25];
    float4 v[25];
    [unroll] for (int k = 0; k < 25; ++k)
    {
        const float2 at = uv + float2(k % 5 - 2, k / 5 - 2) * t;
        v[k] = tex2Dlod(src, float4(at, 0.0, 0.0));
        d[k] = dz ? ReShade::GetLinearizedDepth(at) : 0.0;
    }
    float2 sum = 0.0, fsum = 0.0;
    float  ws = 0.0, fws = 0.0;
    [unroll] for (int i = 0; i < 25; ++i)
    {
        const float moving = dot(v[i].xy, v[i].xy) > 0.0 ? 1.0 : 0.0;
        const float surf   = dz && i != 12 ? OnSurface(d[12], d[i], d[24 - i]) : 1.0;
        const float near   = 1.0 - smoothstep(tol, 2.0 * tol, max(abs(v[i].x - c.x), abs(v[i].y - c.y)));
        const float w      = moving * surf * near * (0.1 + v[i].z);
        sum  += w * v[i].xy; ws += w;
        const float fw     = moving * surf * v[i].z;
        fsum += fw * v[i].xy; fws += fw;
    }
    float2 o = sum / max(ws, 1e-6);
#if BASE_FIN_FILL
    if (dz && fws > 2.0) o = lerp(fsum / fws, o, smoothstep(0.0, 0.5, c.z));
#endif
    return float4(o, c.zw);
}
float4 PS_BaseFinA(float4 vpos : SV_Position, float2 uv : TEXCOORD) : SV_Target { return BaseFin(sDLSS5_ProviderMVPoint, uv, 2.0); }
float4 PS_BaseFinB(float4 vpos : SV_Position, float2 uv : TEXCOORD) : SV_Target { return BaseFin(sDLSS5_Flow1Raw, uv, 2.0); }
float4 PS_BaseFinC(float4 vpos : SV_Position, float2 uv : TEXCOORD) : SV_Target { return tex2Dlod(sDLSS5_Flow1Raw, float4(uv, 0.0, 0.0)); }
// The field the rest of the effect reads is at half resolution; a base that stops above that is
// read up to it bilinearly.
float4 PS_BaseOut3(float4 vpos : SV_Position, float2 uv : TEXCOORD) : SV_Target { return tex2Dlod(sDLSS5_Flow3L, float4(uv, 0.0, 0.0)); }
float4 PS_BaseOut2(float4 vpos : SV_Position, float2 uv : TEXCOORD) : SV_Target { return tex2Dlod(sDLSS5_Flow2L, float4(uv, 0.0, 0.0)); }
float4 PS_BaseAgree6(float4 vpos : SV_Position, float2 uv : TEXCOORD) : SV_Target { return BaseAgree(sDLSS5_Flow6Raw, uv, 64.0); }
float4 PS_BaseAgree5(float4 vpos : SV_Position, float2 uv : TEXCOORD) : SV_Target { return BaseAgree(sDLSS5_Flow5Raw, uv, 32.0); }
float4 PS_BaseAgree4(float4 vpos : SV_Position, float2 uv : TEXCOORD) : SV_Target { return BaseAgree(sDLSS5_Flow4Raw, uv, 16.0); }
float4 PS_BaseAgree3(float4 vpos : SV_Position, float2 uv : TEXCOORD) : SV_Target { return BaseAgree(sDLSS5_Flow3Raw, uv, 8.0); }

// Development only: -Defines FLOWDBG=n puts another stage of the estimator where the rest of the
// effect reads its result. n = 2..6: level n as handed down; 11..16: level n as matched;
// 21..26: level n matched and agreed, before the pooled solve.
#ifdef FLOWDBG
float4 PS_FlowDbg(float4 vpos : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
    #if FLOWDBG == 2
    return tex2Dlod(sDLSS5_Flow2, float4(uv, 0.0, 0.0));
    #elif FLOWDBG == 3
    return tex2Dlod(sDLSS5_Flow3, float4(uv, 0.0, 0.0));
    #elif FLOWDBG == 4
    return tex2Dlod(sDLSS5_Flow4, float4(uv, 0.0, 0.0));
    #elif FLOWDBG == 5
    return tex2Dlod(sDLSS5_Flow5, float4(uv, 0.0, 0.0));
    #elif FLOWDBG == 6
    return tex2Dlod(sDLSS5_Flow6, float4(uv, 0.0, 0.0));
    #elif FLOWDBG == 11
    return tex2Dlod(sDLSS5_Flow1Raw, float4(uv, 0.0, 0.0));
    #elif FLOWDBG == 12
    return tex2Dlod(sDLSS5_Flow2Raw, float4(uv, 0.0, 0.0));
    #elif FLOWDBG == 13
    return tex2Dlod(sDLSS5_Flow3Raw, float4(uv, 0.0, 0.0));
    #elif FLOWDBG == 14
    return tex2Dlod(sDLSS5_Flow4Raw, float4(uv, 0.0, 0.0));
    #elif FLOWDBG == 15
    return tex2Dlod(sDLSS5_Flow5Raw, float4(uv, 0.0, 0.0));
    #elif FLOWDBG == 16
    return tex2Dlod(sDLSS5_Flow6Raw, float4(uv, 0.0, 0.0));
    #elif FLOWDBG == 21
    return FlowAgree(sDLSS5_Flow1Raw, uv, 1.0);
    #elif FLOWDBG == 22
    return FlowAgree(sDLSS5_Flow2Raw, uv, 2.0);
    #elif FLOWDBG == 23
    return FlowAgree(sDLSS5_Flow3Raw, uv, 3.0);
    #elif FLOWDBG == 24
    return FlowAgree(sDLSS5_Flow4Raw, uv, 4.0);
    #elif FLOWDBG == 25
    return FlowAgree(sDLSS5_Flow5Raw, uv, 5.0);
    #elif FLOWDBG == 26
    return FlowAgree(sDLSS5_Flow6Raw, uv, 6.0);
    #elif FLOWDBG == 42
    return float4(tex2Dlod(sDLSS5_Cam5, float4(0.5, 0.5, 0.0, 0.0)).zw / 16.0, 0.0, 0.0);
    #elif FLOWDBG == 43
    return float4(tex2Dlod(sDLSS5_Cam5, float4(0.5, 0.5, 0.0, 0.0)).y * 8.0, tex2Dlod(sDLSS5_Cam4, float4(0.5, 0.5, 0.0, 0.0)).w, 0.0, 0.0);
    #elif FLOWDBG == 47
    return FitIsUsable() ? float4(PredictMV(uv, 0.0) * BUFFER_SCREEN_SIZE, 1.0, 0.0) : float4(0.0, 0.0, 0.0, 0.0);
    #elif FLOWDBG == 48
    return FitIsUsable() ? float4(PredictMV(uv, 0.0) * BUFFER_SCREEN_SIZE, 1.0, 0.0) : tex2Dlod(sDLSS5_Flow1Raw, float4(uv, 0.0, 0.0));
    #elif FLOWDBG == 53
    return tex2Dlod(sDLSS5_Flow3L, float4(uv, 0.0, 0.0));
    #elif FLOWDBG == 44
    return float4(tex2Dlod(sDLSS5_PrevDepthOK, float4(0.5, 0.5, 0.0, 0.0)).x * 0.5, tex2Dlod(sDLSS5_PrevDepthOK, float4(0.5, 0.5, 0.0, 0.0)).y * 60.0, 0.0, 0.0);
    #elif FLOWDBG == 45
    return float4(tex2Dlod(sDLSS5_PrevDepthOK, float4(0.5, 0.5, 0.0, 0.0)).w * 0.25, tex2Dlod(sDLSS5_PrevDepthOK, float4(0.5, 0.5, 0.0, 0.0)).z * 60.0, 0.0, 0.0);
    #elif FLOWDBG == 46
    return float4(ReShade::GetLinearizedDepth(uv) * 60.0, tex2Dlod(sDLSS5_PrevDepth, float4(uv, 0.0, 0.0)).x * 60.0, 0.0, 0.0);
    #elif FLOWDBG == 41
    return float4(ReShade::GetLinearizedDepth(uv) * 60.0, tex2Dlod(ReShade::DepthBuffer, float4(uv, 0.0, 0.0)).x * 60.0, 0.0, 0.0);
    #else
    return 0.0;
    #endif
}
#endif

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
//      nearest whole-pixel vector is taken where it reproduces the pixel and is where the
//      vector all but was already (history is then fetched pixel for pixel).
// What this stage costs when the provider is RIGHT is measured with exact vectors as the
// provider (tools/fx-testbed, fxlab_Oracle.fx; they have to be enabled as a technique, or the
// "exact" vectors are all zero and every conclusion drawn from them is wrong -- it happened):
// they come out of it 0.1 px out where they went in, and the picture is 22% steadier than the
// input under a pan where the same vectors untouched make it 31%. No one rule is to blame;
// each of them takes a little.
// code: 0 static lock, 1 zero, 2 the provider's vector, 3 a neighbouring cell, 4 last frame's
// vector, 5 snapped to whole pixels, 6 the fitted camera motion.
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

// SNAP_NEAR: the whole-pixel vector is only taken where the vector in hand is already within
// this much of it (pixels, per axis). LOCK_YIELD: the static lock stands back where the
// provider's vector reproduces the pixel just as exactly. (Both: see where they are used.)
#ifndef SNAP_NEAR
    #define SNAP_NEAR 0.25
#endif
#ifndef LOCK_YIELD
    #define LOCK_YIELD 1
#endif
#ifndef LOCK_TOL
    #define LOCK_TOL 0.25
#endif
// RDBG (test rig): bits switch single rules of the per-pixel stage off.
//   1 static lock   2 "did not move"   4 neighbouring cells / last frame   32 distrust
#ifndef RDBG
    #define RDBG 0
#endif
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
    if (!(RDBG & 1) && P9MaxDiff(c0, p0) <= STATIC_NOISE && P9MaxDiff(c2, p2) <= 0.25 * STATIC_NOISE)
    {
#if LOCK_YIELD
        // Unless the provider's vector says the very same thing about this pixel: last frame,
        // fetched at it, is this pixel too, as exactly. That is a plain surface in a picture
        // that moves slowly -- the wall next to a doorway under a slow pan. Zero is not wrong
        // there and neither is the wall's motion, but a wall whose plain parts stand still
        // while its edges move is a field full of steps, and a temporal pass given that field
        // was less steady than one given the provider's smooth one (fx-testbed, the turning
        // point of a pan: 1.43 times the input's restlessness with the lock, 1.37 without).
        // A caption or a HUD is not this: at the outline of a glyph the motion behind it
        // reproduces nothing. Nor is a still picture in which the provider has found a
        // fraction of a pixel of nothing here and there: the motion has to be what the
        // provider says all around the pixel, not just at it.
        const float2 lv = ProviderSurfaceMV(uv);
        const float2 lr = px * 12.0;
        const float2 l1 = ProviderMV(uv + float2(lr.x, 0.0)), l2 = ProviderMV(uv - float2(lr.x, 0.0));
        const float2 l3 = ProviderMV(uv + float2(0.0, lr.y)), l4 = ProviderMV(uv - float2(0.0, lr.y));
        const float  lt = LOCK_TOL + 0.1 * length(lv * BUFFER_SCREEN_SIZE);
        const float  ld = max(max(length((l1 - lv) * BUFFER_SCREEN_SIZE), length((l2 - lv) * BUFFER_SCREEN_SIZE)),
                              max(length((l3 - lv) * BUFFER_SCREEN_SIZE), length((l4 - lv) * BUFFER_SCREEN_SIZE)));
        if (dot(lv, lv) > 0.0 && ld <= lt
            && dot(l1, l1) > 0.0 && dot(l2, l2) > 0.0 && dot(l3, l3) > 0.0 && dot(l4, l4) > 0.0
            && P9MaxDiff(c0, Patch9(sDLSS5_PrevLuma, uv + lv, px, 0.0)) <= STATIC_NOISE
            && P9MaxDiff(c2, Patch9(sDLSS5_PrevLuma, uv + lv, px * 4.0, 2.0)) <= 0.25 * STATIC_NOISE)
        {
            code = 2.0; stale = 1.0;
            return lv * BUFFER_SCREEN_SIZE;
        }
#endif
        return float2(0.0, 0.0);
    }

    // The provider's own answer for this pixel.
    float2 best = ProviderSurfaceMV(uv) * BUFFER_SCREEN_SIZE;
    code = 2.0;
    // And the camera's: what the motion fitted to the whole frame says this pixel did.
    float2 cam = best;
    bool   has_cam = false;
    if (CamModelOn() && FitIsUsable())
    {
        cam = PredictMV(uv, ReShade::GetLinearizedDepth(uv)) * BUFFER_SCREEN_SIZE;
        has_cam = true;
    }
    // Nothing here to tell one place from another (flat colour, a soft gradient, plain noise):
    // nothing can be shown to be better than the provider's vector, whose estimate comes from
    // the structure around the pixel -- the inside of a moving surface moves with its outline.
    // Where that agrees with the camera's motion to within a pixel or so, the camera's is the
    // more exact of the two.
    if (contrast < 4.0 * FLOW_NOISE)
    {
        stale = 1.0;
        if (has_cam && length(cam - best) <= 1.0 + 0.1 * length(cam)) { code = 6.0; return cam; }
        return best;
    }

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
    // The camera's vector is taken wherever it explains the pixel about as well as the
    // provider's -- within a quarter, which is no more than the match cost differs between two
    // vectors a fraction of a pixel apart. It has two things the provider's does not: it is
    // smooth by construction, and it comes from the whole frame (on a 38 px pan LumeniteFX is
    // 2.3 px out on average, the fit 0.3-0.4). A pixel that moves by itself fails this test and
    // keeps the provider's vector.
    if (has_cam && any(abs(cam - best) > 0.02))
    {
        const float t_cam = PickCost(c0, m0, c2, m2, uv, cam);
        if (t_cam <= t_best * 1.25 + 2.0 * FLOW_NOISE) { best = cam; t_best = t_cam; code = 6.0; }
    }

    // Still: "did not move" explains the pixel at least twice as well as the provider does (by
    // match cost, so the wall under the flickering lamp goes to zero by this too). Twice, not
    // "somewhat better": under a slow pan a provider that is a third of a pixel off and "did
    // not move" are about equally wrong, and a small head start for zero was enough to freeze
    // every edge in the picture (the tile joints and mortar lines of fx-testbed's courtyard, at
    // 0.6 px a frame).
    // There used to be a second way to zero here: "the pixel's own nine samples are where they
    // were". It is true of a caption -- and of a mortar line that slides along itself under a
    // pan. With EXACT vectors as the provider (fxlab_Oracle.fx) that rule alone cost 6% of the
    // scene's accuracy, and with LumeniteFX the picture was 2% steadier without it, while the
    // captions it was written for kept their zero by the rule above (98.6-100% of glyph pixels
    // either way). So it is gone.
    if (!(RDBG & 2) && t_zero < 0.5 * t_best - FLOW_NOISE)
    {
        best = 0.0; t_best = t_zero; code = 1.0;
    }

    // The vector in hand leaves a real part of the pixel unexplained: the border between two
    // motions, or a provider that is simply off here. The provider's whole cells -- the one
    // under the pixel and the four around it, not interpolations of them -- and last frame's
    // vector get a hearing.
    if (!(RDBG & 4) && t_best > 0.3 * contrast + 2.0 * FLOW_NOISE)
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
    if (RDBG & 32) distrust = 0.0;

    // Sub-pixel polish: half a pixel either side on each axis (pixel scale only: this is about
    // the last fraction of a pixel) and the minimum of the curve through those three costs.
    // If that runs into its half pixel limit, once more from there: up to a pixel in all, which
    // is exactly the range a half-resolution estimate cannot see -- every slow pan lives in it.
    const bool zero_now = dot(best, best) == 0.0;
    float e_best    = zero_now ? e_zero : SymCost(c0, m0, uv, best, 1.0, 0.0);
    bool  from_zero = false;
    // (Not on the camera's vector: a 3x3 patch has nothing to teach a fit of the whole frame,
    // and polishing it took a tenth of the pixels it had right to a quarter pixel away again.)
    if (REFINE_SUBPIXEL && MV_SNAP != 1 && code != 6.0)
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
    // And only where the vector in hand is all but whole already. Real whole-pixel motion is
    // found to a fraction of a pixel before this point, so it loses nothing by that; what it
    // shuts out is a vector of 0.6 px being "reproduced" at 1 on one pixel and at 0 on the
    // next wherever the picture is too plain to tell. Those hops were the largest single cost
    // of this stage on a 3D scene: without any snapping fx-testbed's courtyard was 3-7% steadier
    // under every camera move than with it.
    const bool near_whole = all(abs(best - whole) <= SNAP_NEAR);
    if (MV_SNAP == 1 || (MV_SNAP == 0 && tight && near_whole)) { best = whole; code = dot(whole, whole) > 0.0 ? 5.0 : 1.0; }
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
    if (VALIDATE_DEPTH && DepthUsable() && dc < 0.999)
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

// One gradient step (Lucas-Kanade) of a fit sample's vector v (pixels): a 7x7 window of taps
// `cell` pixels apart on mip `lod`, read symmetrically like SymCost (the fraction of v split
// between the two frames, so both carry the same blur), each frame's own mean brightness taken
// out (a flicker is not motion). Returns the corrected vector.
//   lmin: the smaller eigenvalue of the window's gradient matrix, per tap -- how well the window
//         pins motion down in its WEAKEST direction. An edge, however strong, has none along
//         itself, and a sample that only knows the motion across its edge is no use to a fit.
//   res:  what the corrected vector leaves unexplained (rms, luma);  dev: the window's own rms
//         contrast, to hold that against.
float2 TrackStep(float2 uv, float2 v, float cell, float lod, out float lmin, out float res, out float dev)
{
    const float2 px = BUFFER_PIXEL_SIZE;
    const float2 vi = round(v), vf = v - vi;
    float a[81], b[81];
    [loop] for (int k = 0; k < 81; ++k)
    {
        const float2 o = float2(k % 9 - 4, k / 9 - 4) * cell;
        a[k] = tex2Dlod(sDLSS5_CurLuma,  float4(uv + (o - 0.5 * vf) * px, 0.0, lod)).x;
        b[k] = tex2Dlod(sDLSS5_PrevLuma, float4(uv + (o + vi + 0.5 * vf) * px, 0.0, lod)).x;
    }
    float sxx = 0.0, sxy = 0.0, syy = 0.0, sxt = 0.0, syt = 0.0, stt = 0.0;
    float sx = 0.0, sy = 0.0, st = 0.0, sa = 0.0, saa = 0.0;
    [loop] for (int j = 1; j < 8; ++j)
    {
        [loop] for (int i = 1; i < 8; ++i)
        {
            const int   c  = j * 9 + i;
            const float gx = 0.25 * ((a[c + 1] - a[c - 1]) + (b[c + 1] - b[c - 1])) / cell;
            const float gy = 0.25 * ((a[c + 9] - a[c - 9]) + (b[c + 9] - b[c - 9])) / cell;
            const float t  = b[c] - a[c];
            sxx += gx * gx; sxy += gx * gy; syy += gy * gy;
            sxt += gx * t;  syt += gy * t;  stt += t * t;
            sx  += gx;      sy  += gy;      st  += t;
            sa  += a[c];    saa += a[c] * a[c];
        }
    }
    const float n = 49.0;
    sxx -= sx * sx / n; sxy -= sx * sy / n; syy -= sy * sy / n;
    sxt -= sx * st / n; syt -= sy * st / n; stt -= st * st / n;
    dev  = sqrt(max(saa - sa * sa / n, 0.0) / n);
    lmin = 0.5 * (sxx + syy - sqrt((sxx - syy) * (sxx - syy) + 4.0 * sxy * sxy)) / n;
    const float det = sxx * syy - sxy * sxy;
    float2 d = 0.0;
    if (det > 1e-14) d = clamp(-float2(syy * sxt - sxy * syt, sxx * syt - sxy * sxt) / det, -cell, cell);
    res = sqrt(max(stt + d.x * sxt + d.y * syt, 0.0) / n);
    return v + d;
}

// Pass 1: sample the provider's flow and the depth on a sparse grid.
//
// ReShade cannot skip a whole pass on a uniform, so both fit passes start by checking the
// uniforms that decide whether anything downstream will ever read them. It matters:
// PS_FitSolve is a ONE-pixel shader that runs 2 x 920 iterations (6 x 920 for the per-pixel
// stage's camera vector) with two fetches each and a 9x9 Gauss-Jordan solve every time -- a
// long serial chain on a single lane, half a millisecond of it, every frame. Both consumers
// (GEOM_ENABLE, CAMERA_MODEL) are off by default.
// (The built-in estimator is a third consumer, and always on: it starts its cells from the fit,
// see BASE_CAM.)
#if DLSS5_MV_BUILTIN
  #if BASE && BASE_CAM
    #define DLSS5_FIT_ALWAYS 1
  #endif
#endif
#ifndef DLSS5_FIT_ALWAYS
    #define DLSS5_FIT_ALWAYS 0
#endif
bool FitWanted() { return DLSS5_FIT_ALWAYS || GEOM_ENABLE || (CamModelOn() && MV_PIPELINE == 0); }
void PS_FitSamples(float4 vpos : SV_Position, float2 uv : TEXCOORD, out float4 A : SV_Target0, out float4 B : SV_Target1)
{
    if (!FitWanted()) { A = 0.0; B = 0.0; return; }
    float2 suv = (floor(vpos.xy) + 0.5) / float2(DLSS5_FIT_W, DLSS5_FIT_H);
    float2 mv  = ProviderMV(suv);
    bool  valid  = true;
    float weight = 1.0;
    if (FitWanted())
    {
        // For the per-pixel stage's camera vector the samples are made exact first. A provider's
        // errors are not noise that a fit averages away -- LumeniteFX reads a 0.64 px pan as
        // 0.50 nearly everywhere, and a model fitted to that says 0.50 too. So each sample is
        // moved to the most structured of five spots in its cell (a vector on a blank wall is
        // a guess) and TRACKED against the two frames themselves, starting from the provider's
        // vector: one step on 4 px cells to get within reach, three on 2 px cells. It is kept
        // only if its window pins the motion down in both directions, the tracking settled,
        // and the result explains the window -- about one sample in eight, on fx-testbed's
        // courtyard.
        // Half resolution is where it ends on purpose. Steps at full resolution were tried,
        // through the sampler and with a Catmull-Rom spline from whole pixels, and were worse
        // either way (a slow pan right to a quarter pixel on 53% of the frame against 77%):
        // the pixel-scale detail of a rendered 4K frame is not a shifted copy of the last
        // frame's. Even started from EXACT vectors no version of this tracker kept more than
        // about two samples in three within a quarter pixel; it is the fit over all of them
        // that makes them usable.
        // Fitted to exact vectors the model is right to a quarter pixel on 96% of a panning
        // frame (tools/fx-testbed, fxlab_Oracle.fx); fitted to these samples, on 73-83%.
        const float2 px   = BUFFER_PIXEL_SIZE;
        const float2 step = 0.25 / float2(DLSS5_FIT_W, DLSS5_FIT_H);
        // (Structure is judged on 4 px cells: at 4K a 3x3 of single pixels is flat nearly
        // everywhere in a rendered scene, and a first version that asked for contrast there
        // was left with 27 samples of 920.)
        float2 at = (floor(suv * BUFFER_SCREEN_SIZE) + 0.5) * px;
        P9     c0 = Patch9(sDLSS5_CurLuma, at, px * 4.0, 2.0);
        float  detail = P9Contrast(c0, P9Mean(c0));
        [unroll] for (int k = 0; k < 4; ++k)
        {
            const float2 o  = float2(k < 2 ? -1.0 : 1.0, (k & 1) ? 1.0 : -1.0) * step;
            const float2 a2 = (floor((suv + o) * BUFFER_SCREEN_SIZE) + 0.5) * px;
            const P9     c2 = Patch9(sDLSS5_CurLuma, a2, px * 4.0, 2.0);
            const float  d2 = P9Contrast(c2, P9Mean(c2));
            if (d2 > detail) { at = a2; detail = d2; }
        }
        suv = at;
        // What did not change at all is left out: a still HUD is not the camera, and under a
        // slow pan it sits well inside any outlier tolerance -- a fifth of the samples at zero
        // made a 0.64 px pan come out as 0.55. (A still frame leaves nothing to fit, and the
        // per-pixel stage then has no camera vector, which is right: it needs none.)
        const P9 s0 = Patch9(sDLSS5_CurLuma, suv, px, 0.0), s2 = Patch9(sDLSS5_CurLuma, suv, px * 4.0, 2.0);
        valid = !(P9MaxDiff(s0, Patch9(sDLSS5_PrevLuma, suv, px, 0.0)) <= STATIC_NOISE
               && P9MaxDiff(s2, Patch9(sDLSS5_PrevLuma, suv, px * 4.0, 2.0)) <= 0.25 * STATIC_NOISE);
        // Nor anything within the window that the feed pass has marked as a still element over
        // a moving picture (last frame's map: this frame's is written after the fit).
        {
            const float2 h = px * 6.0;
            const float hud = max(tex2Dlod(sDLSS5_PrevHud, float4(suv, 0.0, 0.0)).x,
                              max(max(tex2Dlod(sDLSS5_PrevHud, float4(suv + float2(-h.x, -h.y), 0.0, 0.0)).x, tex2Dlod(sDLSS5_PrevHud, float4(suv + float2(h.x, -h.y), 0.0, 0.0)).x),
                                  max(tex2Dlod(sDLSS5_PrevHud, float4(suv + float2(-h.x,  h.y), 0.0, 0.0)).x, tex2Dlod(sDLSS5_PrevHud, float4(suv + float2(h.x,  h.y), 0.0, 0.0)).x)));
            valid = valid && hud < 0.25;
        }
        if (valid)
        {
            float lmin, res, dev;
            float2 v = ProviderMV(suv) * BUFFER_SCREEN_SIZE;
            float2 v1 = v;
            [loop] for (int it = 0; it < 4; ++it)
            {
                const float lod = it == 0 ? 2.0 : 1.0;
                v  = v1;
                v1 = TrackStep(suv, v, exp2(lod), lod, lmin, res, dev);
            }
            // Gradients are small numbers in a rendered frame: asking for 6 noise floors left 30
            // samples of 920, 1.5 leaves a hundred, and below that the vectors get worse faster
            // than they get more.
            const float  g  = 1.5 * FLOW_NOISE;
            valid = lmin >= g * g && all(abs(v1 - v) <= 0.25) && res <= 0.35 * dev + 2.0 * FLOW_NOISE;
            // The error of a tracked vector goes with one over the gradient energy, so a window
            // with a corner in it counts for more than one with a faint texture.
            const float g1 = 2.0 * FLOW_NOISE;
            weight = lmin / (lmin + g1 * g1);
            mv = v1 * px;
        }
    }
    // (A capture has no depth; the fit without parallax terms must not wait for one.)
    valid = valid && all(abs(mv * BUFFER_SCREEN_SIZE) < 512.0);
    A = float4(suv.x - 0.5, suv.y - 0.5, ParallaxRaw(suv), valid ? weight : 0.0);
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
    if (!FitWanted()) { P0 = 0.0; P1 = 0.0; P2 = 0.0; P3 = 0.0; P4 = 0.0; P5 = 0.0; return; }
    float p[18];
    [unroll] for (int z = 0; z < 18; ++z) p[z] = 0.0;
    float inlier = 0.0, rms = 0.0, used = 0.0;
    const int total = DLSS5_FIT_W * DLSS5_FIT_H;

    // Tracked samples (the per-pixel stage's camera vector) get four more passes that keep only
    // what the one before explains closely, down to a quarter of the motion at the middle of
    // the frame (between 0.2 and 1 px). A moving object is thrown out by the second pass; what
    // sits a little off the model -- something half transparent over the scene, parallax
    // without depth -- by these. The tolerance closes in a step at a time -- 4 px, 2, 1, then
    // twice at the tight one -- so that each pass is judged against a fit that the worst of
    // the samples have already left.
    const int passes = 6;
    float valid = 0.0;
    float wlo = 1e9, whi = -1e9, vsum = 0.0, vn = 0.0;
    [loop] for (int s0 = 0; s0 < total; ++s0)
    {
        const int2   c0 = int2(s0 % DLSS5_FIT_W, s0 / DLSS5_FIT_W);
        const float4 a0 = tex2Dfetch(sDLSS5_FitA, c0);
        if (a0.w > 0.0)
        {
            wlo = min(wlo, a0.z); whi = max(whi, a0.z);
            vsum += length(tex2Dfetch(sDLSS5_FitB, c0).xy * BUFFER_SCREEN_SIZE); vn += 1.0;
        }
    }
    // How much the picture moves, typically: what the last passes' tolerance is a quarter of.
    // (It was the motion at the middle of the frame -- which a camera travelling forwards does
    // not have: the tolerance closed to a fifth of a pixel, the tracked samples are not that
    // good, and every travelling shot was refused.)
    const float vtyp = vsum / max(vn, 1.0);
    const float wk = whi - wlo > 1e-7 ? 1.0 / (whi - wlo) : 0.0;
    if (wlo > 1e8) wlo = 0.0;
    [loop] for (int it = 0; it < 6; ++it)
    {
        if (it >= passes) break;
        const float tight = clamp(0.25 * vtyp, 0.2, 1.0);
        const float limit = it < 2 ? GEOM_OUTLIER_PX : max(tight, it == 2 ? 2.0 : it == 3 ? 1.0 : 0.0);
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
            if (a.w <= 0.0) continue;
            if (it == 0) valid += 1.0;
            float B[9]; DLSS5_BASIS(B, a.x, a.y, (a.z - wlo) * wk)
            if (it > 0)
            {
                float pu = 0.0, pv = 0.0;
                [unroll] for (int i0 = 0; i0 < 9; ++i0) { pu += p[i0] * B[i0]; pv += p[9 + i0] * B[i0]; }
                const float r = length((float2(pu, pv) - b.xy) * BUFFER_SCREEN_SIZE);
                if (r > limit) continue;
                se += r * r;
            }
            ++n;
            int k = 0;
            [unroll] for (int i = 0; i < 9; ++i)
            {
                ru[i] += a.w * B[i] * b.x;
                rv[i] += a.w * B[i] * b.y;
                [unroll] for (int j = i; j < 9; ++j) { M[k] += a.w * B[i] * B[j]; ++k; }
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
        // With depth: a camera has ONE forward speed. It moves a point by (position x speed) /
        // distance on both axes, so the x.w term of the horizontal motion and the y.w term of the
        // vertical one are the same number, and the two cross terms (y.w in the horizontal, x.w
        // in the vertical) do not exist. Left free, those four are fitted to the few samples
        // close to the camera and the model is wrong exactly there, where everything it says
        // is multiplied by the largest w in the frame. So: the shared term is the two estimates
        // weighed by what each rests on, the cross terms are zero, and the other seven of each
        // axis are solved again with that taken off their right-hand sides.
#ifndef FITPHYS
    #define FITPHYS 1
#endif
        if (FITPHYS && CamDepthOn() && wk > 0.0)
        {
            // packed upper triangle: (i, j), i <= j  ->  i * 9 - i * (i - 1) / 2 + j - i
            const float su = M[42], sv = M[44];                       // (7,7), (8,8)
            const float tz = (su * p[7] + sv * p[17]) / max(su + sv, 1e-20);
            float H[63];
            [unroll] for (int i3 = 0; i3 < 7; ++i3)
            {
                [unroll] for (int j3 = 0; j3 < 7; ++j3)
                {
                    const int lo = min(i3, j3), hi = max(i3, j3);
                    H[i3 * 9 + j3] = M[lo * 9 - lo * (lo - 1) / 2 + hi - lo];
                }
                H[i3 * 9 + i3] += 1e-5 * n;
                H[i3 * 9 + 7] = ru[i3] - tz * M[i3 * 9 - i3 * (i3 - 1) / 2 + 7 - i3];
                H[i3 * 9 + 8] = rv[i3] - tz * M[i3 * 9 - i3 * (i3 - 1) / 2 + 8 - i3];
            }
            bool bad = false;
            [loop] for (int col3 = 0; col3 < 7; ++col3)
            {
                int   piv3  = col3;
                float best3 = abs(H[col3 * 9 + col3]);
                [loop] for (int r3 = col3 + 1; r3 < 7; ++r3)
                {
                    const float v3 = abs(H[r3 * 9 + col3]);
                    if (v3 > best3) { best3 = v3; piv3 = r3; }
                }
                if (best3 < 1e-12) { bad = true; break; }
                if (piv3 != col3)
                    [unroll] for (int c3 = 0; c3 < 9; ++c3) { const float t3 = H[col3 * 9 + c3]; H[col3 * 9 + c3] = H[piv3 * 9 + c3]; H[piv3 * 9 + c3] = t3; }
                const float inv3 = 1.0 / H[col3 * 9 + col3];
                [unroll] for (int c4 = 0; c4 < 9; ++c4) H[col3 * 9 + c4] *= inv3;
                [loop] for (int r4 = 0; r4 < 7; ++r4)
                {
                    if (r4 == col3) continue;
                    const float f3 = H[r4 * 9 + col3];
                    if (f3 == 0.0) continue;
                    [unroll] for (int c5 = 0; c5 < 9; ++c5) H[r4 * 9 + c5] -= f3 * H[col3 * 9 + c5];
                }
            }
            if (!bad)
            {
                [unroll] for (int i4 = 0; i4 < 7; ++i4) { p[i4] = H[i4 * 9 + 7]; p[9 + i4] = H[i4 * 9 + 8]; }
                p[7] = tz; p[8] = 0.0; p[16] = 0.0; p[17] = tz;
            }
        }
        used   = n;
        inlier = float(n) / float(total);
        if (it > 0) rms = sqrt(se / max(n, 1));
    }

    P0 = float4(p[0],  p[1],  p[2],  p[3]);
    P1 = float4(p[4],  p[5],  p[6],  p[7]);
    P2 = float4(p[8],  p[9],  p[10], p[11]);
    P3 = float4(p[12], p[13], p[14], p[15]);
    P4 = float4(p[16], p[17], wlo, wk);
    P5 = float4(inlier, rms, used, valid);
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

// Did anything in this 8x8 block of pixels change since the last frame? Every pixel is looked
// at: a frame counts as "the previous one again" only if not one block says yes.
float PS_Changed(float4 vpos : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
    const float2 px = BUFFER_PIXEL_SIZE;
    float d = 0.0;
    [unroll] for (int y = 0; y < 8; ++y)
    [unroll] for (int x = 0; x < 8; ++x)
    {
        const float4 at = float4(uv + (float2(x, y) - 3.5) * px, 0.0, 0.0);
        d = max(d, abs(tex2Dlod(sDLSS5_CurLuma, at).x - tex2Dlod(sDLSS5_PrevLuma, at).x));
    }
    return d > STATIC_NOISE ? 1.0 : 0.0;
}

// One pixel of frame-wide facts.
//   x: the share of the frame the provider reports as moving (more than half a pixel), on a
//      16x9 probe -- "the picture as a whole is in motion", as opposed to "something in it is";
//   y: for how many frames in a row this has been the previous frame again;
//   z: the share of 8x8 blocks that changed.
float4 PS_Global(float4 vpos : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
    float n = 0.0;
    [unroll] for (int y = 0; y < 9; ++y)
    [unroll] for (int x = 0; x < 16; ++x)
        n += any(abs(ProviderMV((float2(x, y) + 0.5) / float2(16.0, 9.0))) > 0.5 * BUFFER_PIXEL_SIZE) ? 1.0 : 0.0;
    float changed = 0.0;
    [unroll] for (int j = 0; j < 4; ++j)
    [unroll] for (int i = 0; i < 4; ++i)
        changed += tex2Dlod(sDLSS5_Changed, float4((float2(i, j) + 0.5) / 4.0, 0.0, 6.0)).x;
    changed /= 16.0;
    const float before = tex2Dlod(sDLSS5_PrevGlobal, float4(0.5, 0.5, 0.0, 0.0)).y;
    return float4(n / 144.0, changed > 0.0 ? 0.0 : min(before + 1.0, 1000.0), changed, 0.0);
}

// Is there a depth buffer, and is it this picture's? One pixel, every frame (see DEPTH_USE).
// It has to vary across the screen, and -- carried back along the vectors found from the
// picture alone -- it has to be what last frame's depth was there, on four fifths of a 16x9
// grid. A frame that passes counts one up; one that fails takes fifteen off, so a cut or a
// loading screen costs a quarter of a second and a buffer that flickers never gets there.
float4 PS_DepthCheck(float4 vpos : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
    float lo = 1e9, hi = -1e9, ok = 0.0, n = 0.0;
    [loop] for (int k = 0; k < 144; ++k)
    {
        const float2 at = (float2(k % 16, k / 16) + 0.5) / float2(16.0, 9.0);
        const float  d  = ReShade::GetLinearizedDepth(at);
        lo = min(lo, d); hi = max(hi, d);
        const float2 was = at + ProviderMV(at);
        const float  in_ = all(was >= 0.0) && all(was <= 1.0) ? 1.0 : 0.0;
        n  += in_;
        ok += in_ * (SameSurface(d, tex2Dlod(sDLSS5_PrevDepth, float4(saturate(was), 0.0, 0.0)).x) > 0.5 ? 1.0 : 0.0);
    }
    const bool   good = hi - lo > 0.004 && n >= 40.0 && ok >= 0.8 * n;
    const float  held = tex2Dlod(sDLSS5_PrevDepthOK, float4(0.5, 0.5, 0.0, 0.0)).x;
    return float4(good ? min(held + 1.0, 120.0) : max(held - 15.0, 0.0), ok / max(n, 1.0), hi - lo, n);
}
float4 PS_DepthKeep(float4 vpos : SV_Position, float2 uv : TEXCOORD) : SV_Target { return tex2Dlod(sDLSS5_DepthOK, float4(0.5, 0.5, 0.0, 0.0)); }

float4 PS_GlobalHistory(float4 vpos : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
    return tex2Dlod(sDLSS5_Global, float4(0.5, 0.5, 0.0, 0.0));
}

#ifndef VOLR_LO
    #define VOLR_LO 0.3
#endif
#ifndef VOLR_HI
    #define VOLR_HI 1.0
#endif
#ifndef VOL_LO
    #define VOL_LO 0.35
#endif
#ifndef VOL_HI
    #define VOL_HI 1.0
#endif
// A pixel that was hidden a frame ago (the depth test says so: where its vector points, last
// frame had something nearer or further) has no history of its own. What it does have is a
// surface, and that surface moved: the pixel is given the motion of the pixels around it that
// are on its surface AND were visible a frame ago -- which is what a game engine hands a
// temporal pass for such a pixel, and what the pass's own disocclusion handling expects.
// REVEAL: 0 = no vector at all (zero), 1 = the vector as it is, 2 = its surface's.
// (It used to be 0. On tools/fx-testbed that made a travelling camera's picture 1.15 times as
// restless as the input where it is now 1.04, and with exact vectors as the provider it was
// the largest single error at outlines: 3.3 px against 1.5.)
#ifndef REVEAL
    #define REVEAL 2
#endif
float2 RevealedMV(float2 uv, float d, float2 own)
{
    float2 sum = 0.0;
    float  ws  = 0.0;
    [unroll] for (int k = 0; k < 16; ++k)
    {
        const float  r  = k < 8 ? 6.0 : 16.0;
        const float  a  = 0.7853982 * k + (k < 8 ? 0.0 : 0.3926991);
        const float2 at = uv + float2(cos(a), sin(a)) * r * BUFFER_PIXEL_SIZE;
        const float  da = ReShade::GetLinearizedDepth(at);
        const float2 v  = ProviderMV(at);
        const float  dp = tex2Dlod(sDLSS5_PrevDepth, float4(at + v, 0.0, 0.0)).x;
        const float  w  = SameSurface(d, da) * SameSurface(da, dp);
        sum += w * v; ws += w;
    }
    return ws > 0.5 ? sum / ws : own;
}

void PS_MotionVectors(float4 vpos : SV_Position, float2 uv : TEXCOORD,
                      out float2 mv_out : SV_Target0, out float mask : SV_Target1,
                      out float depth : SV_Target2, out float static_now : SV_Target3, out float2 vec_now : SV_Target4,
                      out float hud : SV_Target5, out float vol : SV_Target6)
{
    bool drop_history = false;
    hud = 0.0;
    // Providers hand out "delta UV": previous position = uv + mv. DLSS wants the same
    // direction, in pixels.
    const float2 flow = ProviderSurfaceMV(uv);
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
        if (VALIDATE_DEPTH && DepthUsable() && d < 0.999)
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
        if (VALIDATE_DEPTH && DepthUsable() && d < 0.999 && code > 1.5)
        {
            const float2 puv = uv + mv;
            if (all(puv >= 0.0) && all(puv <= 1.0))
            {
                const float dp  = tex2Dlod(sDLSS5_PrevDepth, float4(puv, 0.0, 0.0)).x;
                const float tol = DEPTH_TOLERANCE * max(d, 1e-3);
                const float bad_depth = saturate((abs(dp - d) - tol) / (tol + 1e-5));
                distrust = max(distrust, bad_depth);
#if REVEAL == 0
                if (bad_depth > 0.5) mv = float2(0.0, 0.0);
#elif REVEAL == 2
                if (bad_depth > 0.5) mv = RevealedMV(uv, d, mv);
#endif
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
#ifdef FITFLAG
    if (FitIsUsable()) mv_px = float2(FITFLAG, 0.0);
#endif
    mv_out = mv_px * MV_SIGN * MV_SCALE;
    mask   = saturate(distrust) * MASK_STRENGTH;
    depth  = RawDepth(uv);

    // For DLSS5_Feed_Restore: is this something no motion explains? Two signs.
    // One, the pixel changed and the best vector there is leaves a real part of that standing
    // (distrust): a flame, the edge of drifting smoke, something that just appeared.
    // Two, the motion found around here is a jumble -- neighbouring spots a few pixels apart
    // disagree by more than a part of their own length: hot air bending what is behind it,
    // smoke churning. Taken as the LEAST such disagreement at four places around the pixel,
    // so that it has to hold over an area: the outline of a moving object is a line of
    // disagreement too, and is none of this.
    {
        const float2 r1 = BUFFER_PIXEL_SIZE * 8.0, r2 = BUFFER_PIXEL_SIZE * 14.0;
        float jumble = 1e9;
        [unroll] for (int q = 0; q < 4; ++q)
        {
            const float2 at = uv + float2(q == 0 ? -r2.x : q == 1 ? r2.x : 0.0, q == 2 ? -r2.y : q == 3 ? r2.y : 0.0);
            const float2 c  = ProviderMV(at);
            const float2 a  = 0.25 * (ProviderMV(at + float2(r1.x, 0.0)) + ProviderMV(at - float2(r1.x, 0.0)) + ProviderMV(at + float2(0.0, r1.y)) + ProviderMV(at - float2(0.0, r1.y)));
            jumble = min(jumble, length((c - a) * BUFFER_SCREEN_SIZE) / (0.6 + 0.1 * length(c * BUFFER_SCREEN_SIZE)));
        }
        vol = max(saturate(2.0 * distrust), smoothstep(VOL_LO, VOL_HI, jumble));
        // Three, and the plainest: last frame, moved by the vector this pixel is given, does not
        // have this frame's SHAPE here. A flame is followed quite happily by a motion estimator
        // (it licks upward, and so do the vectors), and what it was a frame ago still is not
        // what it is now; smoke thickens and thins in place.
        // Shape, not level: the five samples are compared as ratios (logarithms), with the
        // ratio they have in common taken off. A lamp that flickers on a still wall changes
        // every sample by the same factor and is none of this -- a temporal pass may smooth
        // that as it likes. And against the patch's own contrast, because on fine texture a
        // vector a fraction of a pixel out leaves a difference too.
        const float2 pv = mv_px * BUFFER_PIXEL_SIZE;
        float lc[5], ld[5];
        float lm = 0.0;
        [unroll] for (int q2 = 0; q2 < 5; ++q2)
        {
            const float2 o = float2(q2 == 1 ? -3.0 : q2 == 2 ? 3.0 : 0.0, q2 == 3 ? -3.0 : q2 == 4 ? 3.0 : 0.0) * BUFFER_PIXEL_SIZE;
            lc[q2] = log(max(tex2Dlod(sDLSS5_CurLuma, float4(uv + o, 0.0, 0.0)).x, 1e-3));
            ld[q2] = lc[q2] - log(max(tex2Dlod(sDLSS5_PrevLuma, float4(uv + o + pv, 0.0, 0.0)).x, 1e-3));
            lm += ld[q2] / 5.0;
        }
        float left = 0.0, own = 0.0;
        [unroll] for (int q3 = 0; q3 < 5; ++q3) { left += abs(ld[q3] - lm) / 5.0; own += abs(lc[q3] - lc[0]) / 4.0; }
        vol = max(vol, smoothstep(VOLR_LO, VOLR_HI, (left - 0.01) / (0.5 * own + 0.03)));
    }
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
                          : code < 3.5 ? float3(0.0, 0.6, 0.6) : code < 4.5 ? float3(0.8, 0.0, 0.7) : code < 5.5 ? float3(0.85, 0.75, 0.0) : float3(0.9, 0.4, 0.0);
        return saturate(img + tint);
    }
    if (DEBUG_VIEW == 10)
    {
        const float2 v = tex2Dlod(sDLSS5_MV, float4(uv, 0.0, 0.0)).xy;
        // Whole 10-bit codes, 1/8 px a step and +-63 px, so a zero vector reads back as exactly
        // 512. (It used to be 1/16 px and +-31 px, and a 38 px pan read back as 30 from every
        // provider, exact vectors included -- an "underestimate" that was only this clamp.)
        return float3((round(clamp(v, -63.0, 63.0) * 8.0) + 512.0) / 1023.0, tex2Dlod(sDLSS5_Vol, float4(uv, 0.0, 0.0)).x);   // + the volatile mark, for tools/fx-testbed
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

// Nine bilinear taps `width` pixels apart: at 1.5, about a 5x5 binomial.
float3 Blur5(sampler2D s, float2 uv, float width)
{
    const float2 o = BUFFER_PIXEL_SIZE * width;
    float3 c = tex2Dlod(s, float4(uv, 0.0, 0.0)).rgb * 0.25;
    c += (tex2Dlod(s, float4(uv + float2( o.x, 0.0), 0.0, 0.0)).rgb + tex2Dlod(s, float4(uv + float2(-o.x, 0.0), 0.0, 0.0)).rgb
        + tex2Dlod(s, float4(uv + float2(0.0,  o.y), 0.0, 0.0)).rgb + tex2Dlod(s, float4(uv + float2(0.0, -o.y), 0.0, 0.0)).rgb) * 0.125;
    c += (tex2Dlod(s, float4(uv + float2( o.x,  o.y), 0.0, 0.0)).rgb + tex2Dlod(s, float4(uv + float2(-o.x,  o.y), 0.0, 0.0)).rgb
        + tex2Dlod(s, float4(uv + float2( o.x, -o.y), 0.0, 0.0)).rgb + tex2Dlod(s, float4(uv + float2(-o.x, -o.y), 0.0, 0.0)).rgb) * 0.0625;
    return c;
}

// How much of its fine texture has the output lost here? 0 = none worth acting on, 1 = clearly.
// The contrast (mean absolute deviation of luma) of a 5x5 of both pictures. There is no test
// for "texture, not an edge" on purpose: a step that the model has antialiased keeps nearly
// all of its 5x5 contrast (the step is still there) and falls under the threshold by itself,
// while a gradient-direction test threw out the very case this is for -- perforated metal seen
// at a shallow angle is five pixels a period one way and two the other, which reads as lines.
float TextureLoss(float2 uv)
{
    const float2 px = BUFFER_PIXEL_SIZE;
    const float3 lw = float3(0.299, 0.587, 0.114);
    float a[25], b[25];
    float ma = 0.0, mb = 0.0;
    [unroll] for (int k = 0; k < 25; ++k)
    {
        const float4 at = float4(uv + float2(k % 5 - 2, k / 5 - 2) * px, 0.0, 0.0);
        a[k] = dot(tex2Dlod(sDLSS5_Before, at).rgb, lw);
        b[k] = dot(tex2Dlod(ReShade::BackBuffer, at).rgb, lw);
        ma += a[k]; mb += b[k];
    }
    ma /= 25.0; mb /= 25.0;
    float ca = 0.0, cb = 0.0;
    [unroll] for (int k1 = 0; k1 < 25; ++k1) { ca += abs(a[k1] - ma); cb += abs(b[k1] - mb); }
    const float loss = 1.0 - cb / max(ca, 1e-6);
    return smoothstep(0.02, 0.10, loss) * smoothstep(1.0, 2.0, ca / 25.0 / max(RESTORE_TEXTURE_CONTRAST, 1e-4));
}

// The low-pass for texture: sixteen bilinear taps, all BETWEEN pixels (half a pixel and two and
// a half out on each axis), which makes it 0.10 0.10 0.15 0.30 0.15 0.10 0.10 across seven
// pixels. Every tap averages two pixels, so a pattern two pixels a period -- perforated metal
// seen at a shallow angle -- cancels exactly; five pixels a period leaves 7%. Blur5 at any
// width lets a third to a half of such patterns through (its taps land on pixel centres), and
// what gets through counts as "broad" and stays the model's: the moire, at half strength.
float3 BlurWide(sampler2D s, float2 uv)
{
    const float2 px = BUFFER_PIXEL_SIZE;
    float3 c = 0.0;
    [unroll] for (int j = 0; j < 4; ++j)
    [unroll] for (int i = 0; i < 4; ++i)
    {
        const float2 o = float2(i < 2 ? (i == 0 ? -2.5 : -0.5) : (i == 2 ? 0.5 : 2.5), j < 2 ? (j == 0 ? -2.5 : -0.5) : (j == 2 ? 0.5 : 2.5));
        const float  k = ((i == 0 || i == 3) ? 0.2 : 0.3) * ((j == 0 || j == 3) ? 0.2 : 0.3);
        c += k * tex2Dlod(s, float4(uv + o * px, 0.0, 0.0)).rgb;
    }
    return c;
}

float4 PS_Restore(float4 vpos : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
    // The previous frame again (a 30 fps game at 60): shown as it was shown then. Not for
    // more than three in a row -- that is a picture standing still, and it should settle.
    const float repeats = tex2Dlod(sDLSS5_Global, float4(0.5, 0.5, 0.0, 0.0)).y;
    if (RESTORE_HOLD_REPEATS && repeats > 0.5 && repeats < 3.5) return tex2Dfetch(sDLSS5_LastOut, int2(vpos.xy));
    float4 now = tex2Dlod(ReShade::BackBuffer, float4(uv, 0.0, 0.0));
    // The mark, grown by a pixel (the antialiased rim of a glyph belongs to it) and then
    // SMOOTHED over a 5x5: the mark is decided pixel by pixel, on contrast, so inside a fat
    // stroke or on a textured still picture it comes out as a mosaic of marked and unmarked
    // pixels, and a weight that follows it swaps the detail of two different pictures from
    // one pixel to the next. With a neural pass that had redrawn the surface, HDR screenshots
    // from Elgato Studio showed exactly that: checkerboard and block patterns on a parchment,
    // stripes and coloured sparkle inside bright lettering. The weight now changes over a few
    // pixels, never between neighbours.
    const float2 px = BUFFER_PIXEL_SIZE;
    float h = 0.0, wsum = 0.0;
    [unroll] for (int y = -2; y <= 2; ++y)
    [unroll] for (int x = -2; x <= 2; ++x)
    {
        const float k = (3.0 - abs(float(x))) * (3.0 - abs(float(y)));   // 1 2 3 2 1, squared
        float m = 0.0;
        [unroll] for (int v = -1; v <= 1; ++v)
        [unroll] for (int u = -1; u <= 1; ++u)
            m = max(m, tex2Dlod(sDLSS5_Hud, float4(uv + float2(x + u, y + v) * px, 0.0, 0.0)).x);
        h += k * m; wsum += k;
    }
    h /= wsum;
    const float w_hud = smoothstep(0.35, 0.85, h) * RESTORE_STRENGTH;
    const float3 was  = tex2Dlod(sDLSS5_Before, float4(uv, 0.0, 0.0)).rgb;
    // What came in black goes out black (the whole pixel: there is no detail in black to keep).
    if (RESTORE_BLACK)
    {
        const float k = 1.0 - smoothstep(0.004, 0.012, max(was.r, max(was.g, was.b)));
        if (k > 0.0) now.rgb = lerp(now.rgb, was, k);
    }
    // What no motion explains goes out as it came in (see RESTORE_VOLATILE). The mark is read
    // smoothed over about ten pixels, so the hand-over is gradual.
    if (RESTORE_VOLATILE == 1)
    {
        float v = 0.0;
        [unroll] for (int vy = -1; vy <= 1; ++vy)
        [unroll] for (int vx = -1; vx <= 1; ++vx)
            v += tex2Dlod(sDLSS5_Vol, float4(uv + float2(vx, vy) * 4.0 * px, 0.0, 0.0)).x * (2.0 - abs(float(vx))) * (2.0 - abs(float(vy))) / 16.0;
        const float w_vol = smoothstep(0.15, 0.55, v) * RESTORE_STRENGTH;
        if (w_vol > 0.0) now.rgb = lerp(now.rgb, was, w_vol);
    }
    if (RESTORE_WHAT == 1 && w_hud > 0.0) return float4(lerp(now.rgb, was, w_hud), now.a);
    // And texture the model has softened, marked or not (always fine detail only: the pixel
    // is moving, and what the model did to it at any broader scale is its to keep).
    float w = w_hud;
    if (RESTORE_TEXTURE == 1 && w < 1.0) w = max(w, TextureLoss(uv) * RESTORE_STRENGTH);
    if (w <= 0.0) return now;
    const bool   hud    = w_hud > 0.0;
    const float3 lo_was = hud ? Blur5(sDLSS5_Before, uv, RESTORE_WIDTH)      : BlurWide(sDLSS5_Before, uv);
    const float3 lo_now = hud ? Blur5(ReShade::BackBuffer, uv, RESTORE_WIDTH) : BlurWide(ReShade::BackBuffer, uv);
    const float3 mixed  = now.rgb + w * ((was - lo_was) - (now.rgb - lo_now));
    // Never outside what the two pictures hold here. Swapping one picture's detail onto the
    // other's base overshoots wherever the two disagree about the base -- a neural pass that
    // dims or tints a white caption leaves the input's full-strength strokes standing on a
    // lower floor. In SDR that is a slightly-too-bright rim nobody sees. On an HDR10 back buffer
    // the values are PQ codes, where a few percent of overshoot near white is several times the
    // light, and what clips at code 1.0 is 10,000 nits: white text came out peppered with
    // full-brightness specks (Elgato Studio, HDR, with the neural pass; not visible in an SDR
    // capture of the same frame, which is how it got past the test rig).
    const float3 floor_ = min(min(now.rgb, was), min(lo_now, lo_was));
    const float3 ceil_  = max(max(now.rgb, was), max(lo_now, lo_was));
    return float4(clamp(mixed, floor_, ceil_), now.a);
}

float4 PS_KeepShown(float4 vpos : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
    return tex2Dfetch(ReShade::BackBuffer, int2(vpos.xy));
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
    pass PyrP1         { VertexShader = PostProcessVS; PixelShader = PS_PyrP1;         RenderTarget0 = DLSS5_PrevL1; }
    pass PyrC1         { VertexShader = PostProcessVS; PixelShader = PS_PyrC1;         RenderTarget0 = DLSS5_CurL1; }
    pass PyrP2         { VertexShader = PostProcessVS; PixelShader = PS_PyrP2;         RenderTarget0 = DLSS5_PrevL2; }
    pass PyrC2         { VertexShader = PostProcessVS; PixelShader = PS_PyrC2;         RenderTarget0 = DLSS5_CurL2; }
    pass PyrP3         { VertexShader = PostProcessVS; PixelShader = PS_PyrP3;         RenderTarget0 = DLSS5_PrevL3; }
    pass PyrC3         { VertexShader = PostProcessVS; PixelShader = PS_PyrC3;         RenderTarget0 = DLSS5_CurL3; }
    pass PyrP4         { VertexShader = PostProcessVS; PixelShader = PS_PyrP4;         RenderTarget0 = DLSS5_PrevL4; }
    pass PyrC4         { VertexShader = PostProcessVS; PixelShader = PS_PyrC4;         RenderTarget0 = DLSS5_CurL4; }
    pass PyrP5         { VertexShader = PostProcessVS; PixelShader = PS_PyrP5;         RenderTarget0 = DLSS5_PrevL5; }
    pass PyrC5         { VertexShader = PostProcessVS; PixelShader = PS_PyrC5;         RenderTarget0 = DLSS5_CurL5; }
    pass PyrP6         { VertexShader = PostProcessVS; PixelShader = PS_PyrP6;         RenderTarget0 = DLSS5_PrevL6; }
    pass PyrC6         { VertexShader = PostProcessVS; PixelShader = PS_PyrC6;         RenderTarget0 = DLSS5_CurL6; }
#if BASE
    pass BaseMatch6    { VertexShader = PostProcessVS; PixelShader = PS_BaseMatch6;    RenderTarget0 = DLSS5_Flow6Raw; }
    pass BaseAgree6    { VertexShader = PostProcessVS; PixelShader = PS_BaseAgree6;    RenderTarget0 = DLSS5_Flow6; }
    pass BaseMatch5    { VertexShader = PostProcessVS; PixelShader = PS_BaseMatch5;    RenderTarget0 = DLSS5_Flow5Raw; }
    pass BaseAgree5    { VertexShader = PostProcessVS; PixelShader = PS_BaseAgree5;    RenderTarget0 = DLSS5_Flow5; }
    pass BaseMatch4    { VertexShader = PostProcessVS; PixelShader = PS_BaseMatch4;    RenderTarget0 = DLSS5_Flow4Raw; }
    pass BaseAgree4    { VertexShader = PostProcessVS; PixelShader = PS_BaseAgree4;    RenderTarget0 = DLSS5_Flow4; }
    pass FlowKeep      { VertexShader = PostProcessVS; PixelShader = PS_FlowKeep;      RenderTarget0 = DLSS5_FlowKeep; }
    pass BaseMatch3    { VertexShader = PostProcessVS; PixelShader = PS_BaseMatch3;    RenderTarget0 = DLSS5_Flow3Raw; }
    pass BaseAgree3    { VertexShader = PostProcessVS; PixelShader = PS_BaseAgree3;    RenderTarget0 = DLSS5_Flow3; }
#else
    pass Flow6         { VertexShader = PostProcessVS; PixelShader = PS_Flow6;         RenderTarget0 = DLSS5_Flow6Raw; }
    pass FlowEq6       { VertexShader = PostProcessVS; PixelShader = PS_FlowEq6;       RenderTarget0 = DLSS5_FlowEqA6; RenderTarget1 = DLSS5_FlowEqB6; RenderTarget2 = DLSS5_FlowStA6; }
    pass FlowR1_6      { VertexShader = PostProcessVS; PixelShader = PS_FlowR16;       RenderTarget0 = DLSS5_FlowStB6; }
    pass FlowR2_6      { VertexShader = PostProcessVS; PixelShader = PS_FlowR26;       RenderTarget0 = DLSS5_FlowStA6; }
    pass FlowR3_6      { VertexShader = PostProcessVS; PixelShader = PS_FlowR36;       RenderTarget0 = DLSS5_FlowStB6; }
    pass FlowFill6     { VertexShader = PostProcessVS; PixelShader = PS_FlowFill6;     RenderTarget0 = DLSS5_Flow6; }
    pass Flow5         { VertexShader = PostProcessVS; PixelShader = PS_Flow5;         RenderTarget0 = DLSS5_Flow5Raw; }
    pass FlowEq5       { VertexShader = PostProcessVS; PixelShader = PS_FlowEq5;       RenderTarget0 = DLSS5_FlowEqA5; RenderTarget1 = DLSS5_FlowEqB5; RenderTarget2 = DLSS5_FlowStA5; }
    pass FlowR1_5      { VertexShader = PostProcessVS; PixelShader = PS_FlowR15;       RenderTarget0 = DLSS5_FlowStB5; }
    pass FlowR2_5      { VertexShader = PostProcessVS; PixelShader = PS_FlowR25;       RenderTarget0 = DLSS5_FlowStA5; }
    pass FlowR3_5      { VertexShader = PostProcessVS; PixelShader = PS_FlowR35;       RenderTarget0 = DLSS5_FlowStB5; }
    pass FlowFill5     { VertexShader = PostProcessVS; PixelShader = PS_FlowFill5;     RenderTarget0 = DLSS5_Flow5; }
    pass Flow4         { VertexShader = PostProcessVS; PixelShader = PS_Flow4;         RenderTarget0 = DLSS5_Flow4Raw; }
    pass FlowEq4       { VertexShader = PostProcessVS; PixelShader = PS_FlowEq4;       RenderTarget0 = DLSS5_FlowEqA4; RenderTarget1 = DLSS5_FlowEqB4; RenderTarget2 = DLSS5_FlowStA4; }
    pass FlowR1_4      { VertexShader = PostProcessVS; PixelShader = PS_FlowR14;       RenderTarget0 = DLSS5_FlowStB4; }
    pass FlowR2_4      { VertexShader = PostProcessVS; PixelShader = PS_FlowR24;       RenderTarget0 = DLSS5_FlowStA4; }
    pass FlowR3_4      { VertexShader = PostProcessVS; PixelShader = PS_FlowR34;       RenderTarget0 = DLSS5_FlowStB4; }
    pass FlowFill4     { VertexShader = PostProcessVS; PixelShader = PS_FlowFill4;     RenderTarget0 = DLSS5_Flow4; }
    pass FlowKeep      { VertexShader = PostProcessVS; PixelShader = PS_FlowKeep;      RenderTarget0 = DLSS5_FlowKeep; }
    pass Flow3         { VertexShader = PostProcessVS; PixelShader = PS_Flow3;         RenderTarget0 = DLSS5_Flow3Raw; }
    pass FlowEq3       { VertexShader = PostProcessVS; PixelShader = PS_FlowEq3;       RenderTarget0 = DLSS5_FlowEqA3; RenderTarget1 = DLSS5_FlowEqB3; RenderTarget2 = DLSS5_FlowStA3; }
    pass FlowR1_3      { VertexShader = PostProcessVS; PixelShader = PS_FlowR13;       RenderTarget0 = DLSS5_FlowStB3; }
    pass FlowR2_3      { VertexShader = PostProcessVS; PixelShader = PS_FlowR23;       RenderTarget0 = DLSS5_FlowStA3; }
    pass FlowR3_3      { VertexShader = PostProcessVS; PixelShader = PS_FlowR33;       RenderTarget0 = DLSS5_FlowStB3; }
    pass FlowFill3     { VertexShader = PostProcessVS; PixelShader = PS_FlowFill3;     RenderTarget0 = DLSS5_Flow3; }
#endif
#if BASE == 2
    pass BaseOut       { VertexShader = PostProcessVS; PixelShader = PS_BaseOut3;      RenderTarget0 = DLSS5_Flow1; }
#elif BASE == 3
    pass BaseMatch2    { VertexShader = PostProcessVS; PixelShader = PS_BaseMatch2;    RenderTarget0 = DLSS5_Flow2Raw; }
    pass BaseAgree2    { VertexShader = PostProcessVS; PixelShader = PS_BaseAgree2;    RenderTarget0 = DLSS5_Flow2; }
    pass BaseOut       { VertexShader = PostProcessVS; PixelShader = PS_BaseOut2;      RenderTarget0 = DLSS5_Flow1; }
#elif BASE == 4
    pass BaseMatch2    { VertexShader = PostProcessVS; PixelShader = PS_BaseMatch2;    RenderTarget0 = DLSS5_Flow2Raw; }
    pass BaseAgree2    { VertexShader = PostProcessVS; PixelShader = PS_BaseAgree2;    RenderTarget0 = DLSS5_Flow2; }
    pass BaseMatch1    { VertexShader = PostProcessVS; PixelShader = PS_BaseMatch1;    RenderTarget0 = DLSS5_Flow1Raw; }
    pass BaseAgree1    { VertexShader = PostProcessVS; PixelShader = PS_BaseAgree1;    RenderTarget0 = DLSS5_Flow1; }
#if BASE_FIN >= 1
    pass BaseFinA      { VertexShader = PostProcessVS; PixelShader = PS_BaseFinA;      RenderTarget0 = DLSS5_Flow1Raw; }
  #if BASE_FIN >= 2
    pass BaseFinB      { VertexShader = PostProcessVS; PixelShader = PS_BaseFinB;      RenderTarget0 = DLSS5_Flow1; }
  #else
    pass BaseFinC      { VertexShader = PostProcessVS; PixelShader = PS_BaseFinC;      RenderTarget0 = DLSS5_Flow1; }
  #endif
#endif
#else
    pass Flow2         { VertexShader = PostProcessVS; PixelShader = PS_Flow2;         RenderTarget0 = DLSS5_Flow2Raw; }
    pass FlowEq2       { VertexShader = PostProcessVS; PixelShader = PS_FlowEq2;       RenderTarget0 = DLSS5_FlowEqA2; RenderTarget1 = DLSS5_FlowEqB2; RenderTarget2 = DLSS5_FlowStA2; }
    pass FlowR1_2      { VertexShader = PostProcessVS; PixelShader = PS_FlowR12;       RenderTarget0 = DLSS5_FlowStB2; }
    pass FlowR2_2      { VertexShader = PostProcessVS; PixelShader = PS_FlowR22;       RenderTarget0 = DLSS5_FlowStA2; }
    pass FlowR3_2      { VertexShader = PostProcessVS; PixelShader = PS_FlowR32;       RenderTarget0 = DLSS5_FlowStB2; }
    pass FlowFill2     { VertexShader = PostProcessVS; PixelShader = PS_FlowFill2;     RenderTarget0 = DLSS5_Flow2; }
    pass Flow1         { VertexShader = PostProcessVS; PixelShader = PS_Flow1;         RenderTarget0 = DLSS5_Flow1Raw; }
    pass FlowEq1       { VertexShader = PostProcessVS; PixelShader = PS_FlowEq1;       RenderTarget0 = DLSS5_FlowEqA1; RenderTarget1 = DLSS5_FlowEqB1; RenderTarget2 = DLSS5_FlowStA1; }
    pass FlowR1_1      { VertexShader = PostProcessVS; PixelShader = PS_FlowR11;       RenderTarget0 = DLSS5_FlowStB1; }
    pass FlowR2_1      { VertexShader = PostProcessVS; PixelShader = PS_FlowR21;       RenderTarget0 = DLSS5_FlowStA1; }
    pass FlowR3_1      { VertexShader = PostProcessVS; PixelShader = PS_FlowR31;       RenderTarget0 = DLSS5_FlowStB1; }
    pass FlowFill1     { VertexShader = PostProcessVS; PixelShader = PS_FlowFill1;     RenderTarget0 = DLSS5_Flow1; }
#endif
#ifdef FLOWDBG
    pass FlowDbg       { VertexShader = PostProcessVS; PixelShader = PS_FlowDbg;       RenderTarget0 = DLSS5_Flow1; }
#endif
#endif
    pass DepthCheck    { VertexShader = PostProcessVS; PixelShader = PS_DepthCheck;    RenderTarget0 = DLSS5_DepthOK; }
    pass FitSamples    { VertexShader = PostProcessVS; PixelShader = PS_FitSamples;    RenderTarget0 = DLSS5_FitA; RenderTarget1 = DLSS5_FitB; }
    pass FitSolve      { VertexShader = PostProcessVS; PixelShader = PS_FitSolve;      RenderTarget0 = DLSS5_Cam0; RenderTarget1 = DLSS5_Cam1; RenderTarget2 = DLSS5_Cam2; RenderTarget3 = DLSS5_Cam3; RenderTarget4 = DLSS5_Cam4; RenderTarget5 = DLSS5_Cam5; }
    pass Changed       { VertexShader = PostProcessVS; PixelShader = PS_Changed;       RenderTarget0 = DLSS5_Changed; }
    pass Global        { VertexShader = PostProcessVS; PixelShader = PS_Global;        RenderTarget0 = DLSS5_Global; }
    pass Guides        { VertexShader = PostProcessVS; PixelShader = PS_MotionVectors; RenderTarget0 = DLSS5_MV; RenderTarget1 = DLSS5_Mask; RenderTarget2 = DLSS5_Depth; RenderTarget3 = DLSS5_StaticNow; RenderTarget4 = DLSS5_VecNow; RenderTarget5 = DLSS5_Hud; RenderTarget6 = DLSS5_Vol; }
    pass History       { VertexShader = PostProcessVS; PixelShader = PS_StoreHistory;  RenderTarget0 = DLSS5_PrevLuma; RenderTarget1 = DLSS5_PrevDepth; RenderTarget2 = DLSS5_PrevMV; RenderTarget3 = DLSS5_PrevStatic; RenderTarget4 = DLSS5_PrevHud; RenderTarget5 = DLSS5_Before; }
    pass GlobalHistory { VertexShader = PostProcessVS; PixelShader = PS_GlobalHistory; RenderTarget0 = DLSS5_PrevGlobal; }
    pass DepthKeep     { VertexShader = PostProcessVS; PixelShader = PS_DepthKeep;     RenderTarget0 = DLSS5_PrevDepthOK; }
    DLSS5_MV_REQUEST_PASS   // Launchpad only: ask it to compute optical flow again next frame
}

technique DLSS5_Feed_Restore
<
    ui_label   = "DLSS 5 Feed - restore text and HUD, hold repeated frames (place BELOW DLSS 5 Feed)";
    ui_tooltip = "Puts the fine detail of the frame as it came in back on captions and HUD elements, after\n"
                 "DLSS and the neural pass have had it, and shows a repeated frame (30 fps content at 60)\n"
                 "exactly as it was shown the first time. Needs the Refine vector pipeline for the first.\n"
                 "Settings are in DLSS5_Feed's own list, under Restore.";
>
{
    pass Restore   { VertexShader = PostProcessVS; PixelShader = PS_Restore; }
    pass KeepShown { VertexShader = PostProcessVS; PixelShader = PS_KeepShown; RenderTarget0 = DLSS5_LastOut; }
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
