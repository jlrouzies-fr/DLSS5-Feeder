/*
    DLSS5_MV_Bridge.fx

    Bridges iMMERSE Launchpad's motion vectors into the community-standard
    texMotionVectors that DLSS5_Feed reads.

    WHY THIS IS NEEDED
    ------------------
    DLSS5_Feed consumes exactly one interface: the shared texMotionVectors
    texture. Launchpad does not write it -- it publishes its vectors as
    Deferred::MotionVectorsTex instead. So Launchpad, despite being the most
    accurate free optical-flow provider for ReShade, is not a drop-in provider
    for the feeder. Two separate reasons, both silent:

    1) NAMESPACE. Launchpad's texture lives in "namespace Deferred". ReShade
       mangles names inside namespaces, so a plain top-level declaration of
       "texture MotionVectorsTex" creates a DIFFERENT resource and binds to
       nothing. The declaration has to be namespaced identically.

    2) PREDICATION. Launchpad computes optical flow only on demand. Its flow
       vertex shader culls the geometry when the feature was not requested:

           if(!Deferred::IPC::is_requested(MARTYSMODS_IPC_FEATURE_OPTICALFLOW))
               o.vpos.xy = -100000;

       Launchpad ends its own technique with IPC_CLEAR(), explicitly so that
       effects running after it can request what they need. Nothing in the
       stock setup does that, so the flow passes never run and the texture
       stays zero. A consumer MUST raise the request itself.

       The request is picked up by Launchpad on the NEXT frame, so in steady
       state the flow is computed every frame.

    NO THIRD-PARTY FILES ARE INCLUDED
    ---------------------------------
    In keeping with this project's stated principle, this shader includes no
    iMMERSE (or any other provider's) files and contains no third-party code.
    Interop is by declaration only -- the same mechanism that makes the
    texMotionVectors convention work: ReShade binds the same resource when a
    texture is declared with a matching qualified name and matching properties.

    The predication buffer is a 1x1 RGBA8 flag, one channel per feature
    (1 = normals, 2 = albedo, 4 = optical flow), written with a single point
    primitive and a render target write mask. The helper shaders below return
    constants -- that is the wire protocol, not borrowed code.

    Vector conventions already agree, so the copy is 1:1: both sides use
    delta UV with prev_uv = uv + mv, both are RG16F at BUFFER_WIDTH x
    BUFFER_HEIGHT. No rescaling is applied.

    USAGE
    -----
    Effect order:   Launchpad  ->  DLSS5_MV_Bridge  ->  DLSS 5 Feed
    Disable any other motion-vector provider (DRME, qUINT) while this is on --
    they write the same texMotionVectors.

    Verify with the "DLSS 5 Feed - debug view" technique: a static scene must
    be flat grey, and moving the camera must flood the frame with colour that
    tracks the direction. Note that the feeder log reports the PRESENCE of
    texMotionVectors, not its contents, so the log alone cannot tell you
    whether vectors are live.
*/

//=============================================================================
// Provider-side resources, declared to match iMMERSE Launchpad
//=============================================================================

namespace Deferred
{
    // Matches mmx_deferred.fxh: delta UV, RG16F, full resolution.
    texture MotionVectorsTex { Width = BUFFER_WIDTH; Height = BUFFER_HEIGHT; Format = RG16F; };
    sampler sBridgeMotionVectors { Texture = MotionVectorsTex; };

    namespace IPC
    {
        // Matches mmx_deferred.fxh: no dimensions given, so this is 1x1 RGBA8.
        // One channel per requestable feature; optical flow is channel 3 (mask 4).
        texture2D PredicationBuffer { Format = RGBA8; };
    }
}

#define BRIDGE_IPC_FEATURE_OPTICALFLOW 4

//=============================================================================
// Consumer-side target, declared to match DLSS5_Feed.fx
//=============================================================================

texture texMotionVectors < pooled = false; > { Width = BUFFER_WIDTH; Height = BUFFER_HEIGHT; Format = RG16F; };

//=============================================================================
// UI
//=============================================================================

uniform float2 BRIDGE_SIGN <
    ui_type = "drag";
    ui_min = -1.0; ui_max = 1.0; ui_step = 2.0;
    ui_label = "Motion vector sign (x, y)";
    ui_tooltip = "Flip a component if the output doubles or smears in that\n"
                 "direction. Both sides use the same convention, so 1, 1 is\n"
                 "normally correct.";
> = float2(1.0, 1.0);

uniform float BRIDGE_SCALE <
    ui_type = "drag";
    ui_min = 0.0; ui_max = 4.0; ui_step = 0.05;
    ui_label = "Motion vector scale";
    ui_tooltip = "1.0 = the provider's estimate as-is. Diagnostic only.";
> = 1.0;

//=============================================================================
// Shaders
//=============================================================================

struct BridgeVSOUT
{
    float4 vpos : SV_Position;
    float2 uv   : TEXCOORD0;
};

// Fullscreen triangle. Written here so the shader pulls in no headers at all;
// BUFFER_WIDTH / BUFFER_HEIGHT are ReShade built-ins.
BridgeVSOUT BridgeVS(in uint id : SV_VertexID)
{
    BridgeVSOUT o;
    o.uv.x = (id == 2) ? 2.0 : 0.0;
    o.uv.y = (id == 1) ? 2.0 : 0.0;
    o.vpos = float4(o.uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return o;
}

float4 BridgeCopyPS(in BridgeVSOUT i) : SV_Target0
{
    float2 mv = tex2Dlod(Deferred::sBridgeMotionVectors, float4(i.uv, 0.0, 0.0)).xy;
    return float4(mv * BRIDGE_SIGN * BRIDGE_SCALE, 0.0, 0.0);
}

// Single point covering the 1x1 predication buffer.
float4 BridgeRequestVS(in uint id : SV_VertexID) : SV_Position
{
    return float4(0.0, 0.0, 0.0, 1.0);
}

float4 BridgeRequestPS(in float4 vpos : SV_Position) : SV_Target0
{
    return 1.0;
}

//=============================================================================

technique DLSS5_MV_Bridge
<
    ui_label   = "DLSS 5 MV Bridge (iMMERSE Launchpad -> texMotionVectors)";
    ui_tooltip = "Place BETWEEN Launchpad and DLSS 5 Feed, and disable any\n"
                 "other motion-vector provider.\n\n"
                 "Also raises Launchpad's optical-flow request, without which\n"
                 "it does not compute flow at all.";
>
{
    pass Copy
    {
        VertexShader = BridgeVS;
        PixelShader  = BridgeCopyPS;
        RenderTarget = texMotionVectors;
    }

    // Request optical flow for the next frame. Launchpad clears the
    // predication buffer at the end of its own technique, so this has to run
    // after it -- which the required effect order already guarantees.
    pass RequestOpticalFlow
    {
        PrimitiveTopology    = POINTLIST;
        VertexCount          = 1;
        VertexShader         = BridgeRequestVS;
        PixelShader          = BridgeRequestPS;
        RenderTarget         = Deferred::IPC::PredicationBuffer;
        RenderTargetWriteMask = BRIDGE_IPC_FEATURE_OPTICALFLOW;
    }
}
