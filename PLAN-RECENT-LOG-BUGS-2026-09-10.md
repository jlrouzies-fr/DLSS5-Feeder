# Plan: recently active bug reports with logs (2026-09-10)

## Scope and selection

Review time: **2026-09-10 07:24 CEST / 05:24 UTC**. The cutoff is the preceding 72 hours:
**2026-09-07 05:24 UTC**.

Included below are open GitHub issues which:

1. were created or updated inside that window;
2. describe broken or regressed behaviour rather than a feature request or general support question; and
3. contain a downloadable log, a log archive, or a gist of logs in the issue or a recent comment.

This selects issues
[#13](https://github.com/jlrouzies-fr/DLSS5-Feeder/issues/13),
[#15](https://github.com/jlrouzies-fr/DLSS5-Feeder/issues/15),
[#44](https://github.com/jlrouzies-fr/DLSS5-Feeder/issues/44),
[#47](https://github.com/jlrouzies-fr/DLSS5-Feeder/issues/47),
[#57](https://github.com/jlrouzies-fr/DLSS5-Feeder/issues/57),
[#62](https://github.com/jlrouzies-fr/DLSS5-Feeder/issues/62),
[#63](https://github.com/jlrouzies-fr/DLSS5-Feeder/issues/63),
[#70](https://github.com/jlrouzies-fr/DLSS5-Feeder/issues/70),
[#74](https://github.com/jlrouzies-fr/DLSS5-Feeder/issues/74),
[#81](https://github.com/jlrouzies-fr/DLSS5-Feeder/issues/81),
[#84](https://github.com/jlrouzies-fr/DLSS5-Feeder/issues/84),
[#85](https://github.com/jlrouzies-fr/DLSS5-Feeder/issues/85),
[#89](https://github.com/jlrouzies-fr/DLSS5-Feeder/issues/89),
[#91](https://github.com/jlrouzies-fr/DLSS5-Feeder/issues/91), and
[#93](https://github.com/jlrouzies-fr/DLSS5-Feeder/issues/93).

Excluded after review:

- #77 is a successful Spore installation report, not a bug.
- #82 asks for NVIDIA NGX/DLSS 5 on an AMD RX 9060 XT. That hardware cannot execute NVIDIA's
  runtime. Current `src/feed_ngx.h` already replaces the misleading old diagnostic with an explicit
  unsupported-adapter verdict.
- #83 has no log attached. Its requested optional host GPU priority was nevertheless implemented in
  0.15.0.
- #90 contains excerpts but no log file. It should be tracked separately as a RenoDX build-classifier
  question.
- #92 says logs are available but does not attach them.
- Closed reports and reports updated only before the exact cutoff are omitted.

## Priority and shared work

| Priority | Issues | Reason |
|---|---|---|
| P0 | #93, #91, #89 | Current 0.15.1 crash or regression; deterministic reproducer exists for #91. |
| P1 | #84, #63/#57, #15, #13 | Live failures with enough evidence or new instrumentation ready for a decisive retest. |
| P2 | #62, #74, #81, #44 | Old-build or externally confounded crashes; establish a current supported reproduction before changing code. |
| Close after confirmation | #70, #85 | Fixes are already in 0.15.0; #85 is reporter-confirmed. |

Before producing another release, add the build commit/hash to the first line of all three logs. Several
threads call two materially different folders “0.14” or “0.15.1”; a semantic version alone cannot prove
which post-tag fixes are present.

## #93 — Dark Souls Remastered crashes in SDR while waiting for D3D12 output

### Evidence

- The 0.15.1 log is healthy through NGX init, feature creation, and more than 1,300 delivered frames.
- The fault is `0xC0000005` in `nvwgf2umx.dll`. The last feeder breadcrumb is `waiting for the D3D12
  result`; the module stack continues through D3D11, DXGI, and the game.
- Windows HDR makes the problem disappear. The failing run uses an SDR
  `B8G8R8A8_UNORM` backbuffer and the D3D11-to-D3D12 Output fallback.
- The current breadcrumb spans both `ID3D11DeviceContext4::Wait` and
  `BlitOutputToBackbuffer` (`src/dlss5-feed.cpp`, D3D11 frame path), so it does not yet locate the
  failing call.

### Fix plan

Implementation started on `fix/recent-log-bugs-2026-09-10`:

- The D3D11 path now records separate breadcrumbs for the cross-API wait, output blit, and D3D11
  state restoration, plus the signal/completed fence values, frame slot, output format, scratch-output
  state, and synchronization mode during the configured diagnostic frames.
- `sync_home=1` now selects a bounded CPU wait for D3D11 as the planned A/B fallback. A timeout reports
  both fence values and checks device removal/DRED before the frame is abandoned.
- A failed `EndCommands()` submission now skips the D3D11 wait and blit instead of using fence value
  zero and potentially presenting stale output.
- The 64-bit add-on builds successfully. The Dark Souls SDR/HDR runtime A/B remains required because
  this workspace cannot reproduce its driver-specific fault locally.

1. Split that breadcrumb into `enqueueing D3D11 wait for D3D12 output`, `blitting output to the game
   backbuffer`, and `restoring D3D11 state`. Record the D3D12 signal value, completed value, frame slot,
   output resource format, and whether `out_scratch` is active.
2. Ask for four short A/B runs on one teleport: `mode=1`; `mode=2`; `mode=2,sync_home=1`; and Windows
   HDR on. `mode=1` retains the transport while removing NGX, and `sync_home=1` replaces the cross-API
   GPU wait with a CPU-confirmed completion. This separates the neural consumer, fence handoff, and
   copy-home blit.
3. Read the attached minidump from the instrumented build. If the fault is in the blit, validate every
   saved/restored D3D11 object and use a private RGBA output plus the existing shader conversion for
   BGRA. If it is in `ctx4->Wait`, switch this path to the already available CPU-fence fallback when the
   adapter/driver combination faults, with a once-per-session warning.
4. Reproduce on driver 616.92 with Dark Souls' SDR BGRA swapchain, including a teleport/reload that
   rebuilds resources. Verify 30 minutes without a driver exception and repeat with HDR to guard the
   working path.

## #91 — 32-bit DXVK loses add-on registration after a throwaway Vulkan instance

### Evidence

- Max Payne 2 creates a launcher Vulkan instance and then the rendering instance. The add-on attaches
  once for the first instance. The second ReShade scan says no add-on registered, while the module stays
  mapped until process exit.
- `DllMain` owns registration and event setup (`src/dlss5-feed32.cpp` near `DllMain`), so a second
  `LoadLibrary` on an already mapped module cannot repeat it.
- The layer-assisted run adds a separate `DXGIDebug.dll` crash. It should not be mixed into the
  registration fix until the normal global-layer path registers correctly.

### Fix plan

1. Extract DllMain's ReShade registration/event setup into idempotent `RegisterReShadeGeneration()` and
   its inverse. Keep process resources, log state, and Vulkan hooks separate from per-ReShade-generation
   state.
2. Add a generation trigger outside `DllMain`. The first prototype should call the registration helper
   when the Vulkan hook observes a new `VkInstance`/device generation and no effect-runtime callback has
   arrived for it. Confirm that `reshade::register_addon(g_self)` succeeds at that point before keeping
   this design; do not assume registration is valid from an arbitrary loader callback.
3. If ReShade rejects late registration, change the 32-bit Vulkan deployment to a small persistent
   bootstrap DLL plus a reloadable ReShade add-on DLL. The bootstrap owns only the Vulkan hook and loads
   a fresh add-on module name for each ReShade generation, ensuring `DllMain` runs.
4. Add a local harness that creates two Vulkan instances/devices in one process, destroys the first
   runtime, and renders through the second. Assert one live callback set, one overlay page, a host
   handshake on instance two, and clean hook teardown.
5. After the normal path passes, retest `layer-x86`. Capture a fresh dump for `DXGIDebug.dll`; remove the
   feed layer from the documented Max Payne route if the two Vulkan layers are redundant and that crash
   remains outside feeder code.

## #89 — 0.15.1 delivers DLSS frames but RenoDX/DFC does not create feature 18

### Evidence

- The client and host logs reach `feature ready` and thousands of evaluated/delivered DLSS frames.
- The matching host ReShade logs show the consumer intercepting feature 1, then feature 18 creation
  failing with `0xBAD00001`. This is the source of “No NR Feature Matched”; it is not a failed feeder
  transport.
- The reporter says a package labelled 0.14 works. The compared installs also contain different runtime
  and consumer combinations, so that observation is not yet a feeder-only bisect.

### Fix plan

1. Preserve one failing folder and one working folder. Inventory hashes and file versions for the feeder,
   host, ReShade, consumer, `_nvngx.dll`, `nvngx_dlss.dll`, and `nvngx_dlssnr.dll`, plus both INI files.
2. Swap only `dlss5-feed-host64.exe` between the folders. If the failure follows it, bisect host commits
   between 0.14.0-beta.5 and 0.15.0, starting with the delayed ReShade-hook wait and pipe pacing changes.
   If it follows the consumer/runtime files, move the report to that component with the exact failing
   hash.
3. Add a host-side consumer outcome line. After the warm-up interval, report one of `neural feature
   active`, `consumer intercepted DLSS but feature 18 failed`, or `consumer did not intercept`. Do not
   let `feature ready` be read as proof that neural rendering ran.
4. Test RenoDX and Deep Fried Chicken independently on driver 616.56 and 616.64 using the same signed
   runtime pair. Keep exactly one consumer in `host64` for each run.
5. Change feeder code only if the host-only binary swap reproduces the regression. The acceptance test is
   a consumer log showing successful feature-18 create/evaluate, not merely feeder frame delivery.

## #84 — Project CARS 3 R10 path; newest report is an OptiScaler redirect failure

### Evidence

- The original R10 typed-UAV hole is already fixed in 0.15.0 by checking the actual requested output
  format and falling back to RGBA8 (`ResolveOutputFormat` and `ResolveOutputFormatHost`).
- The new 0.15.1 log never reaches that code. It detects the OptiScaler DLSS-NR fork with configuration
  that resolves to `EnableDlssInputs=true` and `HookOriginalNvngxOnly=false`, yet the driver answers the
  NGX probe. OptiScaler's load hook did not redirect this feeder's calls.

### Fix plan

1. Reproduce with the exact OptiScaler build and proxy name (`winmm.dll`) from the log. Add timestamps and
   resolved module owners for every NGX entry point before the capability query and before init.
2. Delay the first NGX resolution/probe until OptiScaler reports its load hooks installed, then resolve
   the exports again. Never cache the raw driver address before the proxy has armed.
3. If OptiScaler offers no readiness signal, retry routing once on the next present when the fingerprint
   still says “driver answered”, and stop after one retry to avoid repeated NGX init/shutdown cycles.
4. Verify in Project CARS 3 that the log says calls are routed through OptiScaler, the R10 output is
   converted/fallen back as planned, and OptiScaler reports both its upscaler and neural model active.
5. If re-resolution still returns the driver, file the load-order trace upstream and retain a precise
   feeder warning; the redirect implementation is owned by OptiScaler.

## #63 and #57 — private D3D12 queue/device hangs after successful operation

### Evidence

- Both reports run successfully for thousands of frames, then the allocator ring stops retiring and the
  private D3D12 device is removed with `DXGI_ERROR_DEVICE_HUNG`.
- #63's beta.5 DRED data faults around a `ResourceBarrier`, with no page fault. Beta.5 did not enable
  breadcrumb contexts, so its phase labels were discarded.
- Current 0.15.x code enables breadcrumb contexts, labels copy-in / NGX evaluate / copy-home, names queue
  ownership correctly, and logs fence lag. Neither reporter has supplied a log from that code yet.

### Fix plan

1. Treat these as one technical investigation while retaining both game reports. Obtain 0.15.1 logs from
   FiveM and Batman with `DLSS5_FEED_D3D12_DEBUG=1` and the full DRED phase map.
2. For each game run `mode=1` (transport only), RenoDX, and Deep Fried Chicken. A mode-1 hang belongs to
   feeder barriers/copies; a hang confined to one consumer's `ngx-evaluate` phase belongs to that
   consumer/runtime path.
3. If copy-in or copy-home faults, replace reliance on implicit COMMON promotion/decay for simultaneous
   shared resources with explicit tracked states, and emit the resource name plus before/after state at
   every barrier in debug builds.
4. If the NGX phase faults, retain the feeder's graceful disable and provide the DRED trace to the
   consumer/runtime owner. Do not paper over a stopped queue by increasing `gpu_timeout_ms`.
5. Stress the selected fix for 30 minutes at native resolution and during alt-tab/resolution changes.
   Acceptance requires no allocator timeout, no device removal, and a clean DRED/info queue.

## #15 — 32-bit DXVK pacing collapse with `async_home=1`

### Evidence

- The beta.5 matrix shows stable pacing with `async_home=0` and burst/collapse behaviour with
  `async_home=1`.
- `host_window=1` did not test present cost: both values still create and present the host window.
- Current 0.15.x bounds the pipe backlog, caps present-debt repayment, and logs pipe-write time, host
  backlog, allocator-ring wait, and presents/evaluate. This change has no reporter retest yet.

### Fix plan

1. Retest the same 1080p/240 Hz matrix on 0.15.1 with the background GPU load removed: baseline
   `async_home=1`, `async_home=0`, and `host_gpu_priority=1` only as a diagnostic fourth arm.
2. Use the new paired metrics to choose the next fix: high client pipe-write time means host read
   starvation; high ring wait means GPU scheduling/fence reuse; neither means the present path remains.
3. If the 12-message pipe still creates visible batching, replace the byte-stream frame queue with one
   coalescing “latest pending frame” slot plus a strict ownership/fence contract. Do not deepen the
   allocator ring or silently drop an evaluate while advancing `fence_out`.
4. Verify median and 99th-percentile frame intervals for ten minutes at 1080p and 4K. The result must
   retain `async_home` throughput without rhythmic troughs or stale copied frames.

## #13 — Detroit Vulkan stale capture, plus four smaller confirmed defects

### Evidence

- The original freeze and C++ exception/deadlock are fixed and reporter-confirmed.
- The beta.5 retest identified four feeder defects already fixed in current 0.15.x: mode 2 permanently
  gated when present context is absent, one-way `enabled=0`, false Smooth Motion warnings after a pause,
  and verifier failure with one `ReShadeApps.ini` entry.
- The remaining image failure reproduces with `passthrough=1`, proving NGX is outside the loop. It is a
  stale Vulkan capture or copy-home problem.

### Fix plan

1. Retest current 0.15.1 first to confirm the four shipped fixes. Close those subitems if their new log
   lines appear.
2. Run `passthrough=1` and `passthrough=2` in the same scene. If both freeze, inspect capture; if only 1
   freezes, inspect copy-home.
3. Log swapchain image handle/index, command-buffer handle, queue family, present ID, and the captured
   image hash together for a short diagnostic window. Refuse to read and write the same presented image
   in one frame without an explicit ordering dependency.
4. Correct swapchain-image selection or present-wait propagation based on that trace. Preserve the
   120-frame fallback for installations where ReShade supplies no nested present context.
5. Treat fp16-quantised source depth as upstream/provider behaviour unless a 32-bit-to-32-bit feeder copy
   demonstrably narrows it. Validate moving-scene output for ten minutes and ensure passthrough remains a
   visual no-op.

## #62 — X4 Vulkan crash and driver-level HYPERVISOR_ERROR

### Evidence

- The supplied files are from 0.13.1. The crash occurs with the feeder present even without RenoDX, but
  the log ends during the create-delay period and does not identify a feeder instruction.
- Current code wraps the Vulkan frame path, drains hook calls before removing trampolines, and disables
  the feed after a transport fault. Those changes were made from #62's evidence and remain unverified on
  X4.
- A Windows `HYPERVISOR_ERROR` is below the process boundary; user-mode code cannot catch or recover it.

### Fix plan

1. Retest 0.15.1 with X4 frame generation off, Smooth Motion off, and no neural consumer, first in
   `mode=1`, then `mode=2`. Collect feeder/ReShade logs and a new user-mode dump.
2. Symbolicate the existing and new dumps. Decide whether the instruction is in the feeder Vulkan hook,
   ReShade, the Vulkan loader, or the NVIDIA driver before changing resource ownership code.
3. If the guarded feeder path faults, leave the game alive, log the exact operation, and invalidate the
   current Vulkan generation so no later callback reuses its images/semaphores.
4. If the BSOD remains reproducible, reduce it to the smallest mode-1 sequence and submit the dump plus
   driver version to NVIDIA. Document the affected driver; do not attempt an in-process recovery.

## #74 — Max Payne 1 crash during fullscreen/runtime rebuild

### Evidence

- The newer client log reaches a runtime destroy/recreate and starts an asynchronous host build. The
  game then faults in `maxpayne.exe` reading address `0x6`; the host completes normally and exits cleanly.
- The test still uses driver 610.88. The host's neural feature-requirements query returns `OutOfDate`;
  the documented DLSS 5 neural-rendering minimum is 616.56.
- The breadcrumb says the add-on's worker was waiting for a host build, but that worker uses pipe data and
  does not establish that it corrupted the game's render state.

### Fix plan

1. Require one reproduction on driver 616.56 or newer and 0.15.1. Run `enabled=0`, `mode=1`, and `mode=2`
   around the same fullscreen/loading transition.
2. Attach a monotonically increasing runtime generation to asynchronous connect/build jobs. If the bound
   runtime is destroyed, cancel or discard that job's result before any game-device object is opened or
   copied.
3. Symbolicate the game dump. If `enabled=0` still crashes at the same game address, close as a
   dgVoodoo/game/driver issue. If only mode 1/2 crashes, use the generation trace to locate the stale
   callback or resource.
4. Improve the host verdict for `feature 18 -> OutOfDate`: state that neural rendering is unavailable on
   the installed driver even when plain DLAA init succeeds.

## #81 — `D3D12CreateDevice` fails with `0x887E0003` in The Long Dark

### Evidence

- The attached log is from 0.10.0-beta.2. The shader resolves successfully 4.5 seconds after the early
  “missing” line; it is not the failure.
- The actual failure is `D3D12CreateDevice -> 0x887E0003`, normally the game-local Agility SDK/redist
  folder mismatch now diagnosed by `FeedLogAgilityFolder`.
- Current 0.15.x delays the shader-missing verdict and inventories the local D3D12 folder on this HRESULT.

### Fix plan

1. Request a 0.15.1 log and the exact contents/versions of the game's `D3D12` folder. Confirm whether the
   game exports an Agility SDK version that conflicts with the loaded core.
2. If removing the stale/incomplete redist makes the private device open, document the per-game remedy
   and add it to the verifier. Do not load a second incompatible D3D12Core into the process.
3. If no local redist exists, capture the loaded `d3d12.dll`/`D3D12Core.dll` paths and retry with DRED
   disabled as current code already does. Only then investigate an out-of-process fallback.
4. Acceptance is a current log with a named adapter, successful private-device creation, and no false
   shader-missing warning.

## #44 — Bayonetta crash/resize artefacts

### Evidence

- The newest beta.5 logs show a healthy 32-bit Vulkan transport and Deep Fried Chicken feature-18
  creation/evaluation. The reporter says DFC 1.6.8 works.
- Live host resize and panel-texture regeneration were implemented before beta.5. The newest log shows a
  resize and continued frame delivery.
- A `std::bad_alloc` exception is recorded while signalling the host, but the process continues for more
  than a minute and shuts down cleanly. The record alone is not evidence of the original fatal crash.

### Fix plan

1. Retest 0.15.1 with three repeat resize gestures and confirm the panel texture dimensions change with
   the host window and artefacts clear without restart.
2. Inspect the attached dump's throwing module and stack. If the allocation is caught by its owner and
   the session continues, downgrade that diagnostic from `CRASH RECORDED` to a caught-exception line so
   it does not masquerade as a fatal feeder crash.
3. If a real crash remains, repeat with cast disabled and with `async_home=0`; this separates panel
   resize/cast lifetime from Vulkan handoff.
4. Close after the working-consumer and resize checks pass; split any reproducible caught-exception
   logging defect into a small follow-up.

## #70 — Metro Last Light Redux feature-level 10.1 blit shader failure

### Evidence

- Beta.5 successfully takes the no-shared-UAV fallback, creates all textures, then fails with
  `blit shader creation failed 0x80070057` on a feature-level 10.1 device.
- Root cause is confirmed in current source: the 64-bit path compiled otherwise compatible shaders as
  shader model 5.0. Commit `a96093f` changes blit and FSR shaders to shader model 4.0 and improves the
  failure line.

### Fix plan

1. No further implementation before retest. Ask the reporter to run 0.15.0 or newer and attach the two
   logs from the same session.
2. Verify the log reaches `feature ready` and delivered frames on feature level 10.1, using the private
   output plus shared non-UAV copy.
3. Build and run a forced feature-level-10 test path locally where possible. Keep shader model 4 for the
   basic blit and FSR paths.
4. Close when the reporter confirms; reopen only if the improved line names a different failing object.

## #85 — reduced work resolution fails on sRGB D3D11 swapchains

### Evidence

- The staging texture used the exact `*_UNORM_SRGB` backbuffer format and requested a non-sRGB UNORM SRV,
  which D3D11 rejects unless the resource is typeless.
- Current 0.15.0 creates the staging texture in the typeless family on both 32- and 64-bit paths.
- The reporter has already confirmed that 85% works after updating.

### Fix plan

1. Mark fixed by `a96093f`; no additional code is needed.
2. Add/retain a small resource-creation check covering RGBA8 and BGRA8 sRGB backbuffers at a reduced work
   resolution, verifying a linear UNORM SRV and successful copy.
3. Reply with the fix version and close the issue.

## #47 — NGX returns `PlatformError` only in selected processes

### Evidence

- Recent Watch Dogs A/B logs vary overlays but still predate the matrix diagnostic. They do not separate
  adapter selection, DRED, or feature level.
- Current source provides `DLSS5_FEED_NGX_MATRIX=1`, testing game/default adapter × DRED on/off × feature
  level 11/12 on throwaway devices while preserving the normal session afterward.

### Fix plan

1. Have one reproducible machine run the current build once with `DLSS5_FEED_NGX_MATRIX=1` and all four
   recent overlay configurations. Collect the full matrix, not selected init lines.
2. Follow `DIAGNOSE-47.md`: if one axis flips the result, make only that production-path change. If all
   eight rows fail in a process where host64 succeeds, enumerate loaded modules and compare process
   security/launcher state.
3. Add no more speculative fallbacks before that result. `PlatformError` occurs before feature creation,
   so shader, motion-vector, and output-format changes cannot fix it.
4. Verify any fix on at least two affected games and one known-good control, then disable the matrix by
   default.

## Verification and release order

1. Land diagnostics/lifecycle changes for #93, #91, and #89 behind no behaviour-changing fallback first.
2. Build all four targets with `build.bat` and `build-addon32.bat`; run the host `--test` path for 300/300
   evaluations.
3. Run focused local checks for D3D11 BGRA/sRGB resources, feature level 10 shaders, two-generation Vulkan
   lifecycle, host resize, and 32-bit async handoff.
4. Send instrumented builds to the reporters in priority order. Do not combine results from different
   consumers, runtime DLLs, or drivers without recording hashes.
5. Close #85 immediately after posting the shipped-fix note. Close #70, #44, and the resolved subitems of
   #13 after reporter confirmation. Keep #57 linked to #63 to avoid implementing the same queue-hang fix
   twice.
