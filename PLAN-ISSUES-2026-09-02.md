# Plan: fixes and replies for the issues touched since 2026-09-01 evening

## Context

17 issues moved overnight. Four need code, two need README entries, the rest need a reply or a
close. Decisions taken with the user: implement fixes + docs + replies; mention ShortFuse's
renodx-dlss only as an aside on 64-bit D3D11 reports; I post replies via `gh issue comment`
after the user approves each text. Target release: **0.10.0-beta.3** (already stamped in
`src/dlss5-feed.cpp:59` and `src/dlss5-feed32.cpp:51`, not tagged yet).

Evidence (logs in the scratchpad, code refs verified):

- **#1 Smooth Motion on D3D11** — cptmacp's A/B ReShade logs and Colada-K's SM-on log have the
  same shape: NvPresent64 creates a **second D3D11 device + invisible proxy swapchain**
  (`RegisterClassExA "InvisibleWindowClassNvPresent"` then `CreateSwapChain` on a different
  `pDevice`), so ReShade creates **two effect runtimes** — the first gets `ReShade.ini` (the
  user's preset), the second `ReShade2.ini`. The add-on has a single global `g.runtime` and
  **last `init_effect_runtime` wins** (`src/dlss5-feed.cpp:4843-4862`). `OnReloadedEffects`
  ignores any runtime that is not `g.runtime` (`:4886-4897`), and `OnRenderTechnique` rejects
  it (`:4903`). In both SM-on logs the feed resolves `technique MISSING` on the runtime it
  happened to bind, the effects compile on the other one, and the feed never feeds a frame —
  "healthy looking, no effect", exactly Colada-K's report. Not a pacer problem: a binding bug.
- cptmacp's file is named `renodx-dlss5-4.7.addon64`; `DetectRenodxAddon` (`:232-244`) opens
  the literal name `renodx-dlss5.addon64`, logs "not found", and then treats the engine as
  classic (warm-up re-create on, `EnableHooks` default not written). cptmacp's SM-off run also
  shows `MV probe 0% non-zero` with Lumenite Kernel enabled — a provider problem, separate.
- **#33 NFS MW 2012 (32-bit D3D11)** — `tex 1` = `FEED_OUTPUT` (`src/feed_ipc.h:58`), the
  only slot built with `D3D11_BIND_UNORDERED_ACCESS` (`src/dlss5-feed32.cpp:1195-1199`). Slot
  0 with the identical desc minus UAV succeeded → the device rejects UAV, i.e. a
  **feature-level 10.x device** (Frostbite 2 title). The 32-bit path never reads the feature
  level, never varies the desc on retry (`FeedFail`, `:580-586`), and has no host-creates
  fallback for D3D11 clients (`FeedHostCreatesTextures`, `src/feed_ipc.h:64-67`) although the
  host-side machinery exists (`host/dlss5-feed-host64.cpp:1181-1200`). `mode=1` does not drop
  the UAV (`:1189`, `:1196`) so it is not a workaround.
- **#35 MGSV Ground Zeroes (64-bit D3D11)** — technique presence → `OnRenderTechnique` →
  `FeedFrame11` → `InitSession` on frame 1 (`src/dlss5-feed.cpp:4549-4564`, `:2176-2298`):
  `LoadLibrary(d3d12.dll)`, `D3D12CreateDevice`, `NVSDK_NGX_D3D12_Init` (not SEH-wrapped),
  `SetMultithreadProtected(TRUE)`. All of it runs before `create_delay` and under `mode=1`; only
  `mode=0` is inert. Renaming the technique makes `find_technique` return 0 — consistent with
  the crash being inside `InitSession`. The 64-bit `CrashFilter` (`:124-141`) writes
  `### CRASH RECORDED ###` with module + breadcrumb, so the reporter's log will name the step;
  no log was attached yet.
- **#15 DXVK 32-bit pacing** — feed CPU 0.09–0.17 ms but the frame interval locks at exactly
  33.5 ms in cycles. The host presents its 2-buffer FLIP_DISCARD swapchain **once per evaluate,
  forced, with `Present(0,0)`** (`host/dlss5-feed-host64.cpp:1325`, `:637-680`, `:722-737`);
  no waitable object / `SetMaximumFrameLatency`. When DWM holds a buffer, that Present blocks
  to the next vblank, the game's `async_home` wait on `rs_fence_out` inherits it, and the loop
  settles at 2 vblanks. Also `FeedVkPresentTick` is never called from the 32-bit add-on, so the
  presents-vs-frames probe is silent there.
- **#34 Upscaling** — DLAA is hard-coded (`src/dlss5-feed.cpp:2097-2104`,
  `host/dlss5-feed-host64.cpp:793-822`); `work_resolution` is downsample→DLAA→bilinear back
  (README 416-422, 726-730). No quality-preset key exists; `NREnableUpscaling=1` would only arm
  the RenoDX v4.6 rejection latch. Needs a FAQ entry, not code.

## Code changes

### A. Multi-runtime binding (#1) — `src/dlss5-feed.cpp`

1. Replace "last init wins" with **"the runtime that renders `DLSS5_Feed` wins"**:
   - `OnInitEffectRuntime` / `OnReloadedEffects`: resolve handles for *every* runtime into a
     small fixed table (`{rt, technique, color_var, mv_var, depth_var, mask_var, launchpad,
     handles_ok}`, ≤4 entries) instead of only `g.runtime`. Keep `g.*` as the copy of the bound
     entry. Do not overwrite `g.runtime` on init if it is already bound to a live runtime whose
     handles are OK.
   - `OnRenderTechnique`: match `technique` against the table entry for `rt`; if it is that
     runtime's `DLSS5_Feed` technique and `rt != g.runtime`, adopt it (log
     `"[feed] binding to effect runtime %p (device %p, window class '%s'): it is the one rendering DLSS5_Feed"`),
     then feed. The `dev != g.dev11` session rebuild at `:4551-4563` already handles the device
     switch.
   - `OnDestroyEffectRuntime`: drop the table entry; only clear `g.*` if it was the bound one.
   - Per-runtime log signature in `ResolveHandles` (`:4790-4796`): key the dedupe on `rt` too.
2. Log runtime identity on init: `rt->get_hwnd()` → `GetClassNameA`, `rt->get_device()->get_native()`.
   When the class is `InvisibleWindowClassNvPresent`, say so explicitly: this is the one line
   that tells a user which runtime is the proxy. Prefer never binding to that runtime unless it
   is the only one rendering the technique.
3. Overlay (`:4990` area): show which runtime is bound and how many exist.

### B. Add-on filename match (#1, and future v4.x drops) — `src/dlss5-feed.cpp:232-244`

`FindFirstFileA("renodx-dlss5*.addon64")` in the add-on's directory; if more than one matches,
take the first and warn that several copies are present (ReShade loads them all). Keep the log
line naming the actual file found. Same change for the host (`host/dlss5-feed-host64.cpp`
`HostRenodxDefault` area, ~`:150-160`) which scans `host64\`.

### C. 32-bit D3D11 shared build fallback (#33) — `src/dlss5-feed32.cpp`, `host/dlss5-feed-host64.cpp`

1. Log `g.dev->GetFeatureLevel()` in the `building:` line (`:2374`) and on `MakeShared` failure.
2. On `MakeShared` failure with `E_INVALIDARG` (or on a device below FL 11_0), **fall back to
   host-created textures**: send the build with `b.tex[] = 0` and a new `b.flags |=
   FEED_BUILD_HOST_CREATES` (or reuse the existing rule by extending `FeedHostCreatesTextures`
   with a per-build override), then open the duplicated handles on the D3D11 side with
   `ID3D11Device1::OpenSharedResource1` — the D3D11 side then only needs SRV/RTV binds for
   Color/Depth/MV and **no UAV bind at all** for Output (the game side only copies from it).
   The host already builds and duplicates the set for GL/Vulkan clients (`:1181-1200`); the
   D3D12-side output resource carries `ALLOW_UNORDERED_ACCESS` regardless of the client.
3. If `OpenSharedResource1` is unavailable (no `ID3D11Device1`), `FeedDisable` with a clear
   reason rather than looping at 30 s forever.
4. Protocol: bump nothing if the flag fits the existing `FeedBuild` reserved bits; otherwise
   IPC v5 with the usual mismatch message.

### D. Host present pacing (#15) — `host/dlss5-feed-host64.cpp`

1. Create the disguise swapchain with `DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT`,
   `SetMaximumFrameLatency(1)`, and keep the waitable handle.
2. In `PumpPresent(force=true)` (per evaluate): present only if the waitable handle is
   signalled (`WaitForSingleObject(h, 0)`), otherwise present with `DXGI_PRESENT_DO_NOT_WAIT`
   and treat `DXGI_ERROR_WAS_STILL_DRAWING` as "skip". Never block the serve loop on DWM.
   Keep the once-per-evaluate intent for the DLSS 5 add-on's workset pool (comment at
   `:630-636`) — a skipped present is logged once, not per frame.
3. Signal `fence_out` **before** `PumpPresent` if it isn't already (check `:1315-1325` order).
4. 32-bit add-on: call `FeedVkPresentTick(fed, 120)` from the Vulkan feed path so the
   presents-vs-frames probe works for DXVK users (`src/dlss5-feed32.cpp` Vulkan frame path,
   ~`:2119-2255`).

### E. Crash diagnostics (#35 and every future crash report)

1. `src/dlss5-feed.cpp`: SEH-wrap `NVSDK_NGX_D3D12_Init` (`:2213`, and the ProjectID variant)
   like `SafeCreateDLSS` (`:1312`); on exception log it, `goto fail`, and `FeedDisable`.
   Add breadcrumbs around `LoadLibrary(d3d12.dll)`, `D3D12CreateDevice`, and
   `SetMultithreadProtected`.
2. `CrashFilter` (`:124-141`): write a minidump (`MiniDumpWriteDump`, `MiniDumpWithIndirectlyReferencedMemory`)
   next to the log as `dlss5-feed-crash.dmp`, and log its path. Same filter added to
   `src/dlss5-feed32.cpp` (has `Breadcrumb` but no filter) and `host/dlss5-feed-host64.cpp`.
3. README "Logs and troubleshooting": one bullet on what to attach for a crash (the
   `### CRASH RECORDED ###` line and the `.dmp`).

### F. README (`README.md`)

- **FAQ "Can I use DLSS Quality/Balanced/Performance? (#34)"** under `## Logs and troubleshooting`
  (line 625+): the feeder publishes a 1:1 DLAA contract by construction (no pre-upscale target,
  no jitter, cannot change the game's render size); `work_resolution` is a cost knob, not
  super-resolution; leave `NREnableUpscaling=0`. Cross-link from the `work_resolution` row
  (`:590`) and the Limitations bullet (`:726`).
- **Smooth Motion on D3D11 section (`:60-75`)**: replace "may well be fine" with the actual
  mechanism (proxy swapchain → two ReShade runtimes → beta.3 binds to the one rendering the
  technique), what to look for in the log, and the per-API opt-out as the fallback.
- **Add-on filename**: note that any `renodx-dlss5*.addon64` is recognised (section "Which
  DLSS 5 add-on", `:20-40`).
- **32-bit D3D11**: feature-level 10.x games now go through host-created textures (`## Install
  for a 32-bit game`, `:250`).
- **DXVK**: one paragraph on pacing under `### 32-bit Vulkan (DXVK)` (`:322`): what
  `mode=1`/`async_home=0`/`host_window=0` isolate, and that beta.3 no longer lets the host's
  present block.
- Status table: add rows for Guild Wars Reforged (32-bit DXVK, working, #32), KOTOR (OpenGL,
  fixed 0.9.0, #31), WoW 3.3.5a (32-bit DXVK, #15).

## Replies (posted after the user approves the text; all reference beta.3 where relevant)

| # | Action | Reply gist |
|---|---|---|
| **1** | reply, keep open | Retract the "incomplete install" read for cptmacp — the logs show two ReShade runtimes because NvPresent64 makes its own device+swapchain; the feed bound to the wrong one (code refs). Fix in beta.3. Also: rename `renodx-dlss5-4.7.addon64` → `renodx-dlss5.addon64` until beta.3 (explains "not found"); their SM-off MV probe is 0% → Lumenite Kernel is not producing vectors (check it sits above DLSS 5 Feed and compiled). Ask both reporters: after beta.3, a log taken after 60 s of gameplay (both attached logs end at the effect compile), and whether *other* ReShade effects are visible with SM on (settles whether NvPresent scans out the proxy). Aside: renodx-dlss is the supported route for 64-bit D3D11 if they want to compare. |
| **4, 14, 16** | no reply | Waiting on reporters' beta.2 retests. |
| **6, 8, 13** | no reply | Detroit/Vulkan pacer; already answered. |
| **15** | reply | Thank FIocker for the first working DXVK config (goes in the README). skoriandlp-arch's 33.5 ms lock: the host's per-evaluate `Present` can block on DWM and the game inherits the wait through `async_home` — beta.3 makes it non-blocking. Meanwhile ask for a `mode=1` run (no NGX, isolates transport pacing) and `host_window=0`; note the feed-CPU figure never counts GPU fence waits. |
| **21, 25, 31, 2** | none | Closed / confirmed. |
| **32** | reply + close | Thank; add as a working 32-bit DXVK row; explain that 350→55 is the DLAA-at-native cost plus the cross-process hop, and that beta.3's host pacing fix may help; invite a follow-up log. |
| **33** | reply | Explain `tex 1` = DLSS output, the only UAV texture; slot 0 passed so the device is refusing UAV → feature level 10.x. Not fixable by `mode=1` (same desc). Beta.3 falls back to host-created textures and logs the feature level; ask them to retest and attach `dlss5-feed.log` + `ReShade.log` (ReShade prints the feature level at device creation). |
| **34** | reply + close as answered | DLAA-only by construction, why, `work_resolution` is a cost knob, keep `NREnableUpscaling=0`; link the new README FAQ. |
| **35** | reply | Ask for `dlss5-feed.log` + `ReShade.log` — the log will contain `### CRASH RECORDED ###` with module + last step (list the `InitSession` steps: adapter, D3D12 device, NGX init, multithread protection), and which is the last line. Explain why technique rename/`mode=1`/no add-on made no difference (session opens on technique render, before `create_delay`; only `mode=0` is inert). Beta.3 adds SEH around NGX init and a minidump. Aside: renodx-dlss for 64-bit D3D11. |
| **7** | none | Answered. |

## Files touched

- `src/dlss5-feed.cpp` (A, B, E), `src/dlss5-feed32.cpp` (C, D4, E), `host/dlss5-feed-host64.cpp` (B, C, D, E), `src/feed_ipc.h` (C), `README.md` (F).

## Verification

- Build all three binaries with the existing scripts (see memory `dlss5-feeder-build-verify`; CRLF/BOM caveat).
- A: on the reference machine, a 64-bit D3D11 game with Smooth Motion on (Metro 2033 Redux
  per README) — log must show two runtimes with class names, the bind line, `first frame fed`,
  and NR visibly on. With SM off, unchanged behaviour (single runtime).
- B: rename `renodx-dlss5.addon64` → `renodx-dlss5-4.7.addon64` and confirm the engine line.
- C: force the fallback path with a debug cfg key (`host_creates=1`) on a normal 32-bit D3D11
  game; confirm session + frames; then real retest by the #33 reporter.
- D: WoW/DXVK or any 32-bit game: no regression in frame interval; the host log shows skipped
  presents rather than blocks; #15 reporters retest.
- E: inject a deliberate fault behind a debug key and confirm the `.dmp` lands next to the log.
- Then stamp/tag `v0.10.0-beta.3`, publish the release, and post the approved replies with
  `gh issue comment`.


# Reply drafts — post after 0.11.0-beta.2 is released (each references it)

## #1 (Smooth Motion) — reply, keep open

@cptmacp — I owe you a correction: your setup is not incomplete, and your A/B logs are what found the bug.

Both ReShade logs show what Smooth Motion does on D3D11: NvPresent64 creates a **second D3D11 device and an invisible proxy swapchain** (`RegisterClassExA "InvisibleWindowClassNvPresent"`, then `CreateSwapChain` on a different `pDevice`), and ReShade creates an effect runtime on each — `ReShade.ini` for one, `ReShade2.ini` for the other. The feeder had one global "current runtime" and bound to whichever initialised **last**; under Smooth Motion that was routinely the runtime *without* your preset, so it resolved `DLSS5_Feed.fx technique MISSING` there and then ignored every render of the technique on the other runtime. Healthy-looking log, no neural pass — exactly what @Colada-K described, and their log has the same two-runtime shape.

**0.11.0-beta.2** tracks every runtime and feeds the one that actually renders `DLSS5_Feed`. The log now lists each runtime with its device and window class (the proxy is called out by name) and prints `binding to effect runtime …: it is the one rendering DLSS5_Feed` when it switches.

Two more things from your logs:
- Your add-on file is `renodx-dlss5-4.7.addon64`; the feeder only matched the exact name `renodx-dlss5.addon64`, so it logged "not found" and ran as if a classic engine were installed. beta.2 matches `renodx-dlss5*.addon64`. Until then, renaming the file also fixes it.
- With Smooth Motion off, your run works but `MV probe … 0% non-zero` on every probe: Lumenite Kernel is enabled but producing no vectors, so DLSS is running without motion (static-sharp, smears when moving). That's separate from Smooth Motion — check the Kernel technique sits above DLSS 5 Feed and compiled cleanly in ReShade.log.

Could both of you retest on beta.2 and post `dlss5-feed.log` **after a minute of gameplay** (both attached logs stop at the effect compile, before a frame was fed)? And one question that settles what NvPresent scans out: with Smooth Motion on, are your *other* ReShade effects visible?

(Aside: for 64-bit D3D11 games ShortFuse's renodx-dlss handles presentation natively and needs no feeder — see the banner at the top of the README — if you want a second data point.)

## #15 (32-bit DXVK) — reply

@FIocker — thank you, that is the first end-to-end 32-bit DXVK configuration on record; it is now in the README's DXVK section, and WoW 3.3.5a has a status row.

@skoriandlp-arch — your numbers pointed at the right place. The feeder's `feed CPU` figure only counts CPU time inside the ReShade callback; a GPU-side wait on the helper's fence is invisible to it, which is how 0.09 ms/frame and a rigid 33.5 ms plateau coexist. The helper presented its own (occluded) window **once per evaluate with a blocking `Present`**, and with `async_home` the game's next frame waits on that evaluate's fence — so whenever DWM held the helper's back buffer to the next vblank, the game inherited the wait and settled at two vblanks. **0.11.0-beta.2** makes that present non-blocking (waitable swapchain, latency 1, `DO_NOT_WAIT`; a skipped present is counted in `dlss5-feed-host.log`), and the 32-bit add-on now also feeds the `present probe` counter so an external pacer shows up as presents ≫ frames fed.

If the plateaus survive beta.2, two runs would isolate the remainder: `mode=1` (transport only, no DLSS evaluate) and `async_home=0`, each with `dlss5-feed.log` + `dlss5-feed-host.log`.

## #32 (Guild Wars Reforged) — reply, then close

Thanks — added as a working 32-bit DXVK row in the README. 350+ → 55 fps is the DLAA-at-native cost plus the 32→64-bit hop at 3440x1440; 0.11.0-beta.2 removes one known stall in that hop (the helper's present could block the game, #15), so a quick before/after `dlss5-feed.log` on beta.2 would be welcome, but there is nothing further to fix here. Closing as a working configuration.

## #33 (NFS MW 2012) — reply

`tex 1` is the DLSS **output** — the only one of the four shared textures created with a UAV bind. Slot 0 (Color, identical desc minus the UAV) succeeded, so the device is refusing the UAV itself: that is a **feature-level 10.x D3D11 device**, which cannot bind UAVs (Frostbite 2 creates one). `mode=1` cannot help — it uses the same texture — and the old code retried the same desc forever.

**0.11.0-beta.2** falls back to the route the OpenGL/Vulkan clients use: the 64-bit helper creates the shared set, keeps the output's UAV on its own side and copies the result into a plain shared texture the game opens. The log will say `the game's D3D11 device (feature level 10_x) refused the shared Output texture; the host will create the shared set instead`. Please retest and attach `dlss5-feed.log`, `dlss5-feed-host.log` and `ReShade.log`.

## #34 (Upscaling?) — reply, close as answered

Not possible, and not as a future setting either — the feeder is DLAA-only by construction. Real DLSS upscaling needs the game to render *smaller* than the screen and jitter its camera, then hands DLSS that small frame plus motion vectors. This feeder only ever sees the finished, screen-sized frame ReShade has, so it publishes a 1:1 DLAA contract (input = output size, no jitter). `work_resolution` is a **cost** knob: the frame is downsampled, DLAA + neural rendering run on the smaller image, and the result is expanded back — which is why 50–66% looks blurry rather than like DLSS Quality. Leave the DLSS 5 add-on's `NREnableUpscaling` at 0: with a 1:1 contract it cannot engage, and on v4.6 it parks neural rendering for the run.

For games that ship DLSS already, use the game's own DLSS (or OptiScaler) rather than this feeder; the ~50% cost in a game like BioShock Remastered (@haldi4803) is the neural pass at native resolution and is expected. Written up in the README's troubleshooting FAQ. Closing as answered.

## #35 (MGSV Ground Zeroes) — reply

Thanks for the unusually thorough isolation — it points at one place. The feeder opens its session the moment ReShade renders the `DLSS5_Feed` technique (not when the passes do anything): D3D12 device, NGX init, then multithread protection on the game's immediate context — all before `create_delay`, all under `mode=1` (only `mode=0` is inert), and none of it involves the RenoDX add-on. Renaming the technique is the one change that skips that path entirely, which is exactly what you saw.

Please attach `dlss5-feed.log` and `ReShade.log`: the feeder's log ends with a `### CRASH RECORDED ###` line naming the exception, the module it faulted in, and the step it was on (`opening the D3D12 session` / `initialising NGX on D3D12` / after `D3D11 multithread protection enabled`). **0.11.0-beta.2** additionally catches a fault inside NGX init and turns it into a clean disable, and writes a `dlss5-feed-crash.dmp` next to the log for anything else — please retest on it and include the `.dmp` if one appears.

(Aside: MGSV GZ is 64-bit D3D11, which ShortFuse's renodx-dlss now handles without a feeder — README banner — if you want to compare.)
