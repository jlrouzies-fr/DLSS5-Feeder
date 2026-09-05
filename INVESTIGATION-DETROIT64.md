# Detroit Vulkan: early-submit dependency gap

Base: upstream `v0.14.0-beta.2`, commit `5cd502677ac187959530d2ae679005cdff83880d`.
Experimental build: `0.14.0-beta.2-detroit-sync.1`. Only the 64-bit add-on is changed.

## Finding and confidence

There is a concrete missing dependency at the boundary between ReShade's present
handling and the feeder's early Vulkan flush. A deterministic two-queue GPU test
reproduces stale capture without the dependency and fresh capture with it on the
reported RTX 4070 SUPER / driver 616.56. The reporter subsequently tested this
build in Detroit and confirmed that it works. This is user-reported in-game
validation; the precise diagnostic settings and test duration were not supplied.
DOOM regression testing and the full diagnostic matrix remain outstanding.

The older Smooth Motion conclusion in `PLAN-DETROIT.md` does not settle this new
reproduction. Issue [#13](https://github.com/jlrouzies-fr/DLSS5-Feeder/issues/13)
remains relevant historical evidence; its report is not a test of this patch.
The [beta.2 release](https://github.com/jlrouzies-fr/DLSS5-Feeder/releases/tag/v0.14.0-beta.2)
also contains changes absent from local main, which is why the patch uses the tag.

## Exact control-flow difference

| Operation | mode=1 | mode=2 | mode=2, passthrough=1 |
|---|---|---|---|
| Capture callback RTV, depth, MV | Yes | Yes | Yes |
| Input route | Imported images, Vulkan only | Imported images or linear buffers | Same as mode=2 |
| External ownership release | No | Yes | Yes |
| Flush inside technique callback | No | Yes | Yes |
| Vulkan signal / D3D12 wait | No | Yes | Yes |
| D3D12 per-frame work | None | EvaluateFeature | CopyResource(OUTPUT, COLOR) |
| D3D12 signal / Vulkan wait | No | Yes | Yes |
| Return ownership / reacquire native command buffer | No | Yes | Yes |
| Copy home | Left half of COLOR | OUTPUT, full image by default | Same as mode=2 |

NGX initialization/feature creation are still retained by passthrough to isolate
the per-frame evaluate difference. In unmodified beta.2, differing COLOR/OUTPUT
formats silently fall back to EvaluateFeature even with passthrough enabled.
This build explicitly stops that diagnostic instead of pretending it bypassed
NGX. Use matching formats, confirmed in the log. Restart between transport
configuration tests: beta.2's feature-only rebuild can retain existing textures.

## Why the existing fences do not protect capture

In [ReShade 6.8's Vulkan present hook](https://github.com/crosire/reshade/blob/v6.8.0/source/vulkan/vulkan_hooks_swapchain.cpp),
`present_effect_runtime()` runs before the final immediate-list submission is
connected to `VkPresentInfoKHR::pWaitSemaphores`. The feeder's mode-2 callback
flushes earlier. Its own input fence certifies completion of that early capture,
but cannot make the capture wait for game work on another queue.

The patch captures present context at the device-dispatch entry (including calls
that bypass the loader export). Inside the callback, under ReShade's queue locks,
it queues waits for **all** original binary present semaphores before the first
operation that may flush. It then re-signals those binary semaphores once, so
ReShade can consume its unchanged final wait list. This uses ReShade queue APIs,
without CPU waiting or raw queue submissions in the add-on. The value zero is
ignored for binary semaphores as specified by
[VkTimelineSemaphoreSubmitInfo](https://docs.vulkan.org/refpages/latest/refpages/source/VkTimelineSemaphoreSubmitInfo.html).
The gate runs once per present. An unavailable context or a different callback
command list makes mode 2 skip, with an explicit log; that is not a successful fix.

The first re-signal may flush the already-recorded effects, adding a submission.
Measure performance in-game. `vk_present_sync=0` restores the original dependency
behaviour for a controlled A/B comparison; default is `1`. The existing async-home
experiments should remain off for these comparisons.

## Image identity and lifetime

ReShade derives its current index from the **presented** index before effects.
The most recently acquired image is not necessarily the presented image, so it
would be incorrect to substitute a separately cached acquire index. Its runtime
can also pass a resolved/format-conversion intermediate RTV and later copy that
to the swapchain. Thus RTV != swapchain image alone is not a bug.

The patch keeps the callback RTV and compares its resource mapping across the
flush. It logs the present-request index, corresponding image, capture/home
resources, intermediate status, queue identities, native command buffers and
the feeder timeline values together. It does not claim to measure compositor
scanout or independently hook every acquire call. The direct path should report
`intermediate=0 stable=1 copied=1`; an intermediate path requires the runtime's
later resolve/copy and is labelled explicitly.

A separate lifetime defect was found: `ReleaseFrameResources` drained only the
private D3D12 queue before destroying Vulkan imports. It now also retires Vulkan
use. The device-destruction callback clears the queue pointer because ReShade
has already destroyed queue wrappers at that point.

## The stale hash is consistent with a black centre

Applying the existing hash algorithm to 4096 repetitions of bytes `00 00 00 FF`
produces **`5f9eb3fedcef8383`**, exactly the supplied hash. Therefore the old
64x64 centre sample is consistent with an opaque black area in RGBA8/BGRA8, even
if other pixels are changing. A hash match is not proof of texture-wide staleness.
The old probe also read COLOR **after**, rather than before, evaluation.

With `vk_trace=1`, the new probe samples three 16x16 tiles at quarter, centre and
three-quarter positions every 60 mode-2 frames:

| Stage | Resource / recording point |
|---|---|
| A | Callback source, before capture |
| B | Imported COLOR image after capture, or the populated linear input buffer |
| C | Exact D3D12 pInColor texture, before evaluate/copy |
| D | Exact D3D12 pInOutput texture, after evaluate/copy |
| E | Imported OUTPUT image or active linear output buffer after the return wait |
| F | Copy-home destination after the copy |

F is the swapchain image on the direct path. For an intermediate RTV it is
labelled `F=effect-target`; the final runtime resolve/scanout remains unmeasured.
Probe hashes exclude row padding; CPU reads wait for both APIs. Vulkan buffers
use explicitly coherent memory through ReShade's upload heap because its Vulkan
map API does not invalidate noncoherent readback memory. `uniform=...` marks
spatially constant samples, not a claim about the complete image.

For equal-format passthrough, A=B=C=D=E=F is expected. With NGX, compare A=B=C and
D=E=F separately. Format conversion invalidates byte equality across conversion.
The probe skips historical-output and half-copy modes. It adds a diagnostic
flush once per sample, so also retest with `vk_trace=0` before accepting a fix.

## Validation and next game test

`build.bat`: x64 MSVC compile and link succeeded. `tests/test-vk-present-order.bat`
runs on the real GPU with two queues and a deliberately delayed producer. Eight
iterations each demonstrated fresh deferred capture, stale unguarded early
capture, and fresh gated early capture. All final binary waits retired, including
reuse across frames. This tests the shared wait/re-signal helper used by the patch.
The automated test does not run NGX, validate the ReShade detour in a game, or
test visual flicker. Separately, the reporter confirmed the build works in
Detroit. The six-stage readback logs and DOOM regression remain outstanding.
No 32-bit targets built.

Back up the existing add-on and use `build/dlss5-feed.addon64` next to the game's
executable. Verify the log version suffix `detroit-sync.1`. Preserve the existing
game configuration and use these diagnostic overrides where needed:

```ini
enabled=1
mode=2
passthrough=1
vk_present_sync=1
vk_trace=1
async_home=0
half_home=0
sync_home=0
```

First confirm actual delivered frames and the `ordered=1` identity lines; a
skipped feeder that happens not to flicker is a failed test. Compare passthrough
with sync disabled/enabled, then `passthrough=0` for real DLAA. Pan across a scene
with detail in all three sampled regions for at least 300 delivered frames.
Test both linear-buffer and imported-image transport (`buffer_home=1/0`, with
restarts), and retest the working mode-1 baseline. Repeat the same matrix in
DOOM 2016 Vulkan. Finish with tracing off, then exercise resolution changes,
alt-tab and effect reload. Restore the original add-on to roll back.
