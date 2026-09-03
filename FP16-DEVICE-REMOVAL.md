# The FP16 device removal

*A D3D12 device removed at frame 60, once per run, only when the game presents an FP16 back
buffer. Found and fixed 2026-09-03. This file is the post-mortem, written mostly because the
day it cost was spent on theories that were all wrong.*

---

## Summary

| | |
|---|---|
| **Symptom** | `D3D12 device removed`, reason `DXGI_ERROR_INVALID_CALL` (`0x887A0001`), ~1 second into feeding |
| **Trigger** | The game presents an 8-byte-per-texel back buffer (`R16G16B16A16_FLOAT`) |
| **Cause** | `StaleProbeRecord` copied with a **hardcoded 256-byte row pitch** while using the *actual* swapchain format |
| **Mechanism** | Bad footprint → recording error → `Close()` fails → `EndCommands()` submitted the failed list anyway → runtime removes the device |
| **Fix** | Derive the probe pitch from the format; never execute a command list that failed to close |
| **Blast radius** | Any game with an FP16/scRGB back buffer. Not specific to one title |

Found on **The Surge 2** (Vulkan), whose back buffer is `B8G8R8A8_SRGB` normally — a
[RenoDX](https://github.com/clshortfuse/renodx) HDR add-on upgrades it to `R16G16B16A16_FLOAT` +
scRGB, which is what exposed this. The bug is in the feeder, not in the HDR add-on.

---

## The bug

`StaleProbeRecord` is a diagnostic. Every 60 frames it copies a 64×64 centre block of the colour
input and of the DLSS output into a readback buffer and hashes both, so that when the screen
freezes while every frame still reports "delivered", the log can say *which* hop of the cross-API
transport went stale.

The readback footprint was sized at compile time:

```c
static const UINT kStaleProbeSize  = 64;
static const UINT kStaleProbePitch = 256;   // 64 texels of a 4-byte format, already row-pitch aligned
static const UINT kStaleProbeBlock = kStaleProbePitch * kStaleProbeSize;
static const UINT kStaleProbeEvery = 60;
```

The comment is honest about its assumption, and the assumption held for every format the feeder
had ever met: `R8G8B8A8_UNORM`, `B8G8R8A8_UNORM`, `R10G10B10A2_UNORM`, `R11G11B10_FLOAT` are all
4 bytes per texel, so 64 texels are exactly 256 bytes and that is also D3D12's row-pitch
alignment. Two constraints agreeing by coincidence is what made it invisible.

But the copy itself used the *runtime* format:

```c
dst.PlacedFootprint.Footprint = { g.color_fmt, kStaleProbeSize, kStaleProbeSize, 1, kStaleProbePitch };
```

With `g.color_fmt == DXGI_FORMAT_R16G16B16A16_FLOAT`, one row of 64 texels is **512** bytes.
A `PlacedFootprint` whose `RowPitch` is narrower than `Width × bytes-per-texel` describes a
region that cannot exist, and the runtime rejects it.

## Why a rejected copy removed the device

D3D12 defers command-list recording errors: `CopyTextureRegion` returns `void`, the list is
flagged internally, and `Close()` is where the error surfaces. `EndCommands()` did this:

```c
static UINT64 EndCommands()
{
    g.list->Close();                          // HRESULT discarded
    ID3D12CommandList *lists[] = { g.list };
    g.queue->ExecuteCommandLists(1, lists);   // submits a list in an error state
    ...
}
```

Executing a command list that failed to close is an invalid call, and the runtime's response to
an invalid call is to remove the device. Hence `DXGI_ERROR_INVALID_CALL` as the removal reason.

**One discarded HRESULT turned a harmless diagnostic mistake into a lost device.** That is the
part of this worth remembering; the pitch constant was an ordinary oversight, but the missing
check is what gave it teeth.

---

## Why it took a day

Every early clue pointed away from the real cause. Recorded here so the same trail is not walked
twice.

**"Around frame 60" was read as "around the game's swapchain recreation."** The game recreates its
Vulkan swapchain about a second in — near enough to frame 60 that the two looked like one event.
That framed the whole investigation as a lifetime/interop problem: stale handles, resources
outliving their swapchain, imports surviving a resize. It was actually `kStaleProbeEvery = 60`, a
constant with nothing to do with the swapchain.

**"Only with FP16" was read as a memory-layout problem.** 8 bytes per texel doubles the size of
everything, so the natural suspects were allocation size, row stride and memory type in the
D3D12↔Vulkan import. Those got fixed (see below). It changed nothing, because the failing copy was
into a plain readback buffer that never crossed APIs at all.

**Bisection cleared everything except the probe, and the probe was never a suspect.** Individual
runs ruled out the neural provider (RenoDX's DLSS 5 add-on, Deep Fried Chicken, none),
`buffer_home` 0/1, `sync_home` 0/1, import size, memory type, copy stride, the ReShade FX bridge,
NaNs and negatives in the colour input, and dynamic resolution. The one thing untouched by every
one of those toggles was the diagnostic probe — which reads as "not involved" until you notice it
is also the only thing on a 60-frame period.

**DRED was enabled and reported nothing — and that was the actual breakthrough, misread twice.**

```
[feed] ===== DRED: device removed, reason 0x887A0001 =====
[feed] DRED: GetAutoBreadcrumbsOutput1 failed 0x887A0004
[feed] DRED: GetPageFaultAllocationOutput1 failed 0x887A0004
```

`0x887A0004` is `DXGI_ERROR_UNSUPPORTED` for **both** breadcrumbs and page fault. The instinct is
"DRED isn't working." It was working: there were no breadcrumbs *because nothing ever reached the
GPU*, and no page fault *because there was no fault*. Combined with `INVALID_CALL`, that says the
runtime rejected a call on the CPU side. Every GPU-fault theory was dead at that moment; it took
one more detour to act on it.

**The D3D12 debug layer — the obvious next tool — could not be used.** Installing the Graphics
Tools optional feature got `D3D12SDKLayers.dll` in place and `EnableDebugLayer()` succeeded, but
`D3D12CreateDevice` then failed with `DXGI_ERROR_DEVICE_RESET` (`0x887A0007`) inside the game
process. A standalone test on the same machine and GPU created a device successfully with the
layer enabled, with and without an explicit adapter, and with a device created and released
beforehand — so it is the host process, not the machine. Cause unknown; the layer is now behind
`DLSS5_FEED_D3D12_DEBUG=1` and off by default.

**What finally worked was not a better theory but a cheaper question.** Instead of asking *why*
the device died, ask *which call* killed it: `GetDeviceRemovedReason()` flips synchronously for a
runtime-rejected call, so polling it after each D3D12 call names the offender without any
tooling. Checking `Close()`'s HRESULT was part of the same instrumentation — and that check alone
printed the answer on the first run:

```
[feed] command list Close() failed 0x80070057 -- NOT executing it
[feed]   frame state: 3840x2160 color=10 output=10 home_pitch=30720 home_slice=0 in_pitch=[30720 0 15360 15360] mask_ok=1
```

`0x80070057` is `E_INVALIDARG`; format `10` is `DXGI_FORMAT_R16G16B16A16_FLOAT`; frame 60 exactly.

---

## Lessons

1. **Never discard the HRESULT from `ID3D12GraphicsCommandList::Close()`.** It is the only place a
   recording error appears, and submitting a list that failed to close costs the device. A single
   `if (FAILED(...))` converts the worst outcome in the API into a dropped frame.
2. **A hardcoded pitch next to a runtime format is a latent bug**, even when a comment explains
   the assumption. `kStaleProbePitch = 256` was correct for every format the code had seen and
   wrong the first time it met a new one. Derive it, or assert it.
3. **`INVALID_CALL` + empty DRED means "look on the CPU."** It is not a broken DRED and not a GPU
   fault: the runtime rejected a call, nothing was submitted, so there is nothing to breadcrumb.
4. **Diagnostics are code and can be the bug.** The probe existed to explain freezes and instead
   caused a device removal. Periodic diagnostics deserve suspicion precisely because a periodic
   symptom looks like a rendering event.
5. **Prefer instrumentation that names the culprit over reasoning that predicts it.** Roughly a
   dozen game runs went into eliminating hypotheses. One run with per-call checkpoints answered
   it. When a bug is reproducible, buy the answer instead of deducing it.
6. **A coincidence in timing is not a causal link.** "Frame 60" and "the swapchain is recreated
   about a second in" were unrelated; treating them as one event set the direction for most of
   the day.

---

## What changed

### `src/dlss5-feed.cpp`

**`StaleProbeRecord` / `StaleProbeAnalyse` — the fix.** `StaleProbePitch(DXGI_FORMAT)` derives the
row pitch from `HomeTexelBytes()` and aligns it to `D3D12_TEXTURE_DATA_PITCH_ALIGNMENT`. The
readback buffer is sized for the widest format it will ever see (RGBA32F, 1024-byte rows) so the
two block offsets stay constant, and the bytes actually written per block are recorded at capture
time and used by the hash. A format `HomeTexelBytes` cannot size skips the probe rather than
recording a copy the runtime will reject.

**`EndCommands` — the containment.** `Close()`'s HRESULT is checked. A list that failed to close
is not executed; the failure is logged with the full frame geometry (dimensions, colour and output
formats, home pitch and slice, input pitches), the allocator slot is retired without a submit so
the ring does not wait on a fence value that will never be signalled, and the frame is dropped
through the normal `FeedFail` three-strikes path.

**`CK(label)` — the instrumentation, kept.** Polls `GetDeviceRemovedReason()` after each D3D12 call
in the frame path (`Wait`, each input copy, the barrier block, the NGX evaluate, the copy home,
`ExecuteCommandLists`, both signals) and logs the checkpoint that removed the device plus the last
good one before it. One runtime call per checkpoint. Kept because the class of failure it catches
is intermittent and otherwise nearly unattributable.

**D3D12 debug layer + info queue.** `FeedEnableD3D12DebugLayer` / `FeedAttachInfoQueue` /
`FeedDrainInfoQueue`, drained per frame, on failure, and at removal. Off unless
`DLSS5_FEED_D3D12_DEBUG=1`, because enabling it in this host makes device creation fail.

**DRED.** `FeedEnableDred` / `FeedDumpDred`, hooked into the removal and shared-texture failure
paths. It reported nothing here, but *that* was the decisive signal.

### `src/feed_vk.h`, `src/feed_vk_hook.h`

Correctness work found along the way. **It did not fix this bug** and is committed on its own
merits:

- `FeedVkImportImage` takes the D3D12 allocation size (`GetResourceAllocationInfo`) and binds
  that for the dedicated import, instead of the size Vulkan computed for an image of its own. The
  OpenGL path already did this. A disagreement between the two figures is logged.
- Memory-type selection prefers an allowed type with `VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT`
  instead of taking the lowest set bit. Imported D3D12 default-heap memory is device-local, so
  this states the intent rather than relying on bit order.
- `FeedVkLoad` optionally takes the `VkPhysicalDevice`, which the `vkCreateDevice` hook now
  records, because `vkGetPhysicalDeviceMemoryProperties` needs it and ReShade only exposes the
  `VkDevice`.

---

## Reproducing

1. A game presenting `R16G16B16A16_FLOAT` (any RenoDX scRGB HDR add-on will do it).
2. `mode=2`, feeding normally.
3. Watch frame 60.

Before: `Close()` fails silently, the list is submitted, the device is removed with
`DXGI_ERROR_INVALID_CALL`, DRED reports `UNSUPPORTED` for everything.
After: the probe logs `stale probe (frame 60): colour-in changed, output changed` and the feed
keeps running.
