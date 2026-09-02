// dlss5-feed IPC: the 32-bit in-game add-on <-> 64-bit host protocol.
//
// Everything heavy stays on the GPU. WHICH SIDE CREATES the four shared textures
// depends on the game's API, and the driver decides, not us:
//
//  * D3D11 client (client_kind = FEED_CLIENT_D3D11, protocol v1's only case): the
//    game creates them on D3D11 -- the direction the driver accepts, see the
//    phase-0 spike -- and sends its local NT-handle values; the host duplicates
//    them out of the game process and opens them on D3D12.
//  * OpenGL client (client_kind = FEED_CLIENT_GL) and Vulkan client
//    (FEED_CLIENT_VULKAN): the HOST creates them on D3D12 and duplicates the
//    handles INTO the game, which imports them as GL textures / VkImages. For GL
//    the direction is forced -- GL memory objects are import-only, so a GL process
//    cannot export one (PLAN-OPENGL §5, design A). For Vulkan it is forced the
//    other way round: D3D12 cannot open Vulkan-exported memory. Either way it is
//    the better direction, since the resource is then born with exactly the D3D12
//    flags NGX wants.
//
// Either way the host creates the two shared fences on D3D12 and duplicates them
// INTO the game process. The pipe carries only these fixed-size structs.
//
// Sync per frame n: the host always waits in_fence >= n, evaluates, Signal(out_fence, n).
// The game has two shapes for its half, chosen by its async_home setting:
//
//  * same frame (async_home=0): copy inputs, Signal(in_fence, n), send FeedFrameMsg,
//    record Wait(out_fence, n) + blit. The frame cannot be presented until the host
//    has finished it, so the game's frame time carries the whole round trip.
//  * pipelined (async_home=1, the default): record Wait(out_fence, m) for the frame m
//    sent previously, copy inputs, blit the output the host produced for m, THEN
//    Signal(in_fence, n) and send FeedFrameMsg. The blit reads Output before the
//    signal that lets the host overwrite it, so one shared Output slot stays
//    race-free; the cost is that the DLSS result is one frame old.
//
// Either way it is the same protocol -- the host cannot tell, and does not care.
// A pipe break on either side means "stop feeding".
//
// Version 2 added client_kind and the host-created texture handles. Version 3 added
// FEED_CLIENT_VULKAN and FeedBuildAck::output_fmt -- when the host creates the
// textures it also OWNS the output-format choice (the typed-UAV-store fallback needs
// a D3D12 device to ask), and the game must know the real format to import the
// VkImage and to decide copy-vs-blit for the way home. Version 4 added
// FeedHello::self_process so the host no longer OpenProcess()es the game, which a
// protective DACL on the game process denies (error 5 -- the vanilla-WoW report).
// Version 5 added FeedBuild::client_flags: a D3D11 client whose device refuses the
// shared textures (a feature-level 10.x game, NFS Most Wanted 2012 / issue #33: it
// cannot bind a UAV, and the DLSS output needs one) asks the host to create the set
// instead, exactly as GL/Vulkan clients always have, and to keep the UAV on its own
// side (the host evaluates into a private scratch and copies into the shared Output).
// Version 6 added FeedBuild::target_width/height and FeedFrameMsg::jitter_x/y for
// work_upscale=2 (issue #34): the game asks for a DLSS Super Resolution feature whose
// output is the native size while the guides stay at the work size, and reports the
// sub-pixel shift it applied to the downsample grid on every frame. The host answers
// with FeedBuildAck::flags -- FEED_ACK_SR_UNAVAILABLE when no DLSS quality preset covers
// the ratio, so the game rebuilds as plain DLAA -- and the preset it picked.
//
// Both sides refuse a version they do not understand rather than misparsing it: the
// struct sizes differ between versions, so a mismatched pair would desync the pipe.

#pragma once
#include <cstdint>

#define FEED_IPC_MAGIC   0x35534C44u  // 'DLS5'
#define FEED_IPC_VERSION 6u

// FeedBuild::client_flags (v5+)
#define FEED_BUILD_HOST_CREATES  1u   // tex[] are zero: the host creates the shared set and answers with handles
#define FEED_BUILD_OUTPUT_NO_UAV 2u   // the game's device cannot bind UAVs: share the Output without one

// FeedBuildAck::flags (v6+)
#define FEED_ACK_SR_ACTIVE       1u   // the feature is DLSS Super Resolution work -> target (sr_quality says which preset)
#define FEED_ACK_SR_UNAVAILABLE  2u   // SR was asked for, no preset covers the ratio: the build failed on purpose,
                                      // rebuild with target == work (DLAA)
#define FEED_PIPE_FMT    "\\\\.\\pipe\\dlss5-feed.%lu"   // %lu = game PID

// The bytes a version-1 client sends as its hello: magic, version, pid.
#define FEED_HELLO_V1_SIZE (3u * sizeof(uint32_t))

enum FeedSlot { FEED_COLOR = 0, FEED_OUTPUT, FEED_DEPTH, FEED_MV, FEED_SLOTS };

enum FeedClientKind { FEED_CLIENT_D3D11 = 0, FEED_CLIENT_GL = 1, FEED_CLIENT_VULKAN = 2 };

// True for every client whose API cannot hand D3D12 an importable texture, so the
// host creates the shared set and duplicates the handles in.
static inline bool FeedHostCreatesTextures(uint32_t client_kind)
{
    return client_kind == FEED_CLIENT_GL || client_kind == FEED_CLIENT_VULKAN;
}

#pragma pack(push, 1)

struct FeedHello        // game -> host, once
{
    uint32_t magic;
    uint32_t version;
    uint32_t pid;
    uint32_t client_kind;   // v2+: FeedClientKind. Absent (and 0) from a v1 client.
    uint64_t self_process;  // v4+: a handle to the game process, valid IN THE HOST -- the game
                            // duplicates it in via the process handle CreateProcess gave it.
                            // The host used to OpenProcess(pid), which a protective DACL on the
                            // game process denies (error 5: anti-cheat/DRM, elevation mismatch);
                            // handing the handle over needs no access to the game's DACL at all.
                            // 0 = the duplication failed; the host falls back to OpenProcess.
};

struct FeedHelloAck     // host -> game, once
{
    uint32_t magic;
    uint32_t version;
};

struct FeedBuild        // game -> host, on every resolution/format change
{
    uint32_t width, height;
    uint32_t color_fmt;          // DXGI_FORMAT of the shared Color/Output pair
    uint32_t output_fmt;
    int32_t  hdr;                // resolved flags, not cfg values
    int32_t  depth_inverted;
    int32_t  flags_override;     // -1 = none
    int32_t  transport;          // 1 = no NGX: host copies Color -> Output (cross-process transport test)
    float    mv_scale_x, mv_scale_y;
    uint64_t tex[FEED_SLOTS];    // D3D11 clients: NT-handle VALUES in the game process (host
                                 // duplicates them out). GL/Vulkan clients: all zero -- the host
                                 // creates, and answers with its own handles.
    uint32_t client_flags;       // v5+: FEED_BUILD_* -- a D3D11 client can opt into host-created textures
    uint32_t target_width, target_height;   // v6+: 0 = DLAA (target == width/height). Otherwise the Output
                                            // slot is THIS size and the feature is DLSS Super Resolution
                                            // (work_upscale=2); Color/Depth/MV stay at width/height.
};

struct FeedBuildAck     // host -> game
{
    int32_t  ok;                 // 1 = feature ready
    uint32_t ngx_result;         // NVSDK_NGX_Result of the create (0x1 = success)
    uint64_t fence_in;           // handle values valid in the GAME process (host duplicated them in)
    uint64_t fence_out;
    uint64_t tex[FEED_SLOTS];    // host-creating clients only: handle values valid in the GAME process
    uint64_t tex_size[FEED_SLOTS]; // host-creating clients only: GetResourceAllocationInfo sizes, which
                                   // the GL import needs and a client with no D3D12 device cannot ask for
    uint32_t output_fmt;         // the DXGI_FORMAT the host actually created the Output slot with.
                                 // For a host-creating client this can differ from the requested
                                 // FeedBuild::output_fmt: only the host's device can answer whether
                                 // a typed UAV store to B8G8R8A8 is supported, and where it is not
                                 // the output falls back to R8G8B8A8. Echoed for D3D11 clients too.
    uint32_t flags;              // v6+: FEED_ACK_*
    uint32_t sr_quality;         // v6+: NVSDK_NGX_PerfQuality_Value of an SR feature (FEED_ACK_SR_ACTIVE)
};

struct FeedFrameMsg     // game -> host, per frame
{
    uint64_t n;                  // fence value for this frame
    uint32_t reset;              // 1 = reset temporal history
    float    jitter_x, jitter_y; // v6+: the sub-pixel shift applied to this frame's downsample grid, in
                                 // work pixels; 0 under DLAA. Handed to DLSS as the jitter offset.
};

#pragma pack(pop)
