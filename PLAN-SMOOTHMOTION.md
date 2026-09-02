# Smooth Motion compatibility (issues #1, #10) — there is a fix path

> **Status (2026-09-01, branch `smooth-motion-thread-safety`).** Steps 3, 4 and 5 are implemented
> and all three targets build clean. **Nothing is verified on hardware yet** — neither the
> known-good regression run with Smooth Motion off, nor a run with it on. Steps 1 and 2 (the issue
> replies and the reporter data) are still to do, and the Deferred section stays deferred.
>
> **Update (2026-09-01, later):** first Smooth Motion run is in. Metro 2033 Redux, Smooth Motion
> active, 0.8.0-beta.4: session open, feature ready, 6600+ frames delivered, feed CPU 0.52 ms/frame
> against the 0.50 pre-lock baseline — the serialization costs nothing measurable. No off-thread or
> re-entrant Present was observed in this game, so the guard is armed but untriggered; the Vulkan
> data point (#10) is still needed. The session also root-caused two *external* startup crashes that
> Smooth Motion exposes (Luma's unchecked `GetBuffer(GetCurrentBackBufferIndex())`, fix offered
> upstream; and RenoDX v4.6's `NRStyle=2`, which the feeder now warns about).
>
> **Update (2026-09-01, later still): the Vulkan data point (#10) is in, and it reproduces.**
> **DOOM 2016, 64-bit Vulkan, Smooth Motion ON** — a proven-working row with it off — shows the
> display holding very old frames far past any single-frame lag, while the feeder reports frames
> evaluated and delivered at a normal rate throughout. So the serialization work in steps 3–5 is
> **not sufficient** for Vulkan: the feeder-side race is fixed, but something downstream of our
> copy-home is holding/re-presenting frames. Detroit: Become Human (Demo) shows the identical
> symptom *without* Smooth Motion knowingly enabled — see [`PLAN-DETROIT.md`](PLAN-DETROIT.md)
> for eight tests that rule out the transport, the format, HDR, NR and DLSS itself.
>
> **`DetectSmoothMotion()` cannot see Smooth Motion on Vulkan at all.** It checks for
> `NvPresent64.dll`, the DXGI/D3D implementation; on Vulkan NVIDIA does it inside the ICD's own
> swapchain, and there is no NVIDIA implicit layer either (verified against
> `HKLM\SOFTWARE\Khronos\Vulkan\ImplicitLayers`). So the warning never fires for the exact users
> who need it, and an absent warning must not be read as "Smooth Motion is off" — it was, during
> the Detroit investigation, which cost real time. The durable fix is behavioural: count
> `vkQueuePresentKHR` against frames fed and warn when presents outnumber them, which detects any
> external pacer regardless of vendor or API.
>
> Two things landed that the plan did not call for, both found while implementing:
> - A `CRITICAL_SECTION` is *recursive*, so a lock alone cannot stop a re-entrant Present on the
>   same thread — it would walk straight into a half-built frame. Hence the busy flag beside it.
> - Making the `BeginCommands` timeout non-fatal exposed a latent bug: `fence_event` is auto-reset
>   and a timed-out wait leaves its registration armed, so a later completion can signal it
>   spuriously and let the next frame reset an allocator the GPU is still reading. Every wait on a
>   shared fence event now does `ResetEvent` first and re-checks `GetCompletedValue` after — in the
>   add-on *and* in the host, which had the same hazard already.

## Context

Two open issues report DLSS5-Feeder breaking when NVIDIA Smooth Motion is enabled:

- **#1** — Assassin's Creed Syndicate, 64-bit D3D11, RTX 4070 Laptop, driver 595.79. Feeder alone
  is fine; with Smooth Motion on: visual corruption, *or* neural rendering silently stops (no
  crash, image reverts, stutters).
- **#10** — RDR2, 64-bit Vulkan. Works, then Smooth Motion on → "everything just flickers".

`README.md:39` carries a bare one-line "not compatible" warning with no explanation (commit
`74f38c7`). No runtime detection, no troubleshooting entry, no reply on either issue. The goal is a
**fix**, not a documented limitation.

## What the research actually established

**Smooth Motion is an in-process interposer on Windows, not a pure driver feature.** It is
`NvPresent64.dll`, injected into the game from the DriverStore. It hooks `CreateDXGIFactory*` /
factory vtables, **wraps the game's `IDXGISwapChain` in an `NvPresent` COM wrapper**, drives a
**secondary swapchain** on D3D11, and calls **`Present` more than once per game frame, from its own
pacer thread**. Sources: ReShade's own comment in `source/dxgi/dxgi.cpp` ("External hooks may create
a DXGI factory and rewrite the vtable (e.g. NVIDIA Smooth Motion)"), [ReShade PR
#359](https://github.com/crosire/reshade/pull/359), [Dalamud PR
#2849](https://github.com/goatcorp/Dalamud/pull/2849), and RTSS 7.3.7's changelog (it detects
`nvpresent64.dll` and switches presentation handling).

**Two hypotheses are now dead** — don't spend time on either:

- The reporter's guess in #1 that both hook the same present path. The feeder registers **no**
  present or swapchain events at all (`src/dlss5-feed.cpp:4020-4025`: `create_device`,
  `init_effect_runtime`, `destroy_effect_runtime`, `reshade_reloaded_effects`,
  `reshade_render_technique`, `destroy_device`).
- That `renodx-dlss5`'s global NGX detour adopts Smooth Motion's own NGX feature. The public NGX
  feature enum (`NVSDK_NGX_Feature` in NVIDIA/DLSS `nvsdk_ngx_defs.h`) has **no** Smooth Motion ID,
  and NvPresent is invoked from the driver present path, not the game's NGX client.

**The live hypothesis, and it is a feeder-side bug: the feeder is not thread-safe, and Smooth Motion
is what exposes it.** ReShade's effect chain — and therefore `OnRenderTechnique` → `FeedFrame`
(`:3823`, `:3683`) — runs inside Present. Under Smooth Motion, Present becomes **re-entrant and
off-thread**. The feeder has exactly one lock in the whole add-on, and it is for the log
(`g_log_cs`, `:74`): the entire `g` state struct (`struct Feed`, `:603`), the allocator ring /
`frame_slot` (`:897-926`), the shared textures and the D3D11 immediate context are all unsynchronized, and
nothing calls `ID3D11Multithread::SetMultithreadProtected`. Dalamud hit precisely this and fixed it
with: unwrap the NvPresent COM wrapper, split build-from-composite work, enable multithread
protection on the immediate context, and lock against `ResizeBuffers`.

**A second, independent defect explains "NR silently stops, no crash".** `BeginCommands`
(`:897-914`) waits 2 s on the private queue's fence and on timeout calls `FeedDisable` — a
**permanent latch**. Nothing retries; the only recovery is the overlay's **Re-enable** button
(`:3898`). `Warn()` (`:110-121`) reaches the file log and ReShade's log, but nothing puts the
*reason* in front of the player — the overlay status line (`:3887`) says only
"disabled (see dlss5-feed.log)".

**Already correct — leave it alone.** The feeder writes `EnableHooks=2` (`:231`). RenoDX's public
hook code shows `=1` additionally hooks the Streamline interposer's `CreateDXGIFactory*` /
`D3D12CreateDevice` and wraps the DXGI factory and swapchain — genuinely contested with Smooth
Motion. `=2` is NGX-only. Worth *verifying it lands*, since a user-set value wins (`RenodxDefault`,
`:171`).

**Caveat.** RenoDX ↔ Smooth Motion is a pre-existing generic ReShade-addon conflict
([renodx#318](https://github.com/clshortfuse/renodx/issues/318), open, black screen), and no public
source confirms `renodx-dlss` coexists with Smooth Motion — that is Discord word-of-mouth. Thread
safety may not be sufficient on its own. It is still both necessary and correct.

## Plan

### Step 1 — ship the user-facing workaround immediately (no code)

There is a **per-API** opt-out, better than "turn Smooth Motion off". NVIDIA Profile Inspector DRS
settings (IDs from `Orbmu2k/nvidiaProfileInspector` `CustomSettingNames.xml`):

- **`0xB0CC0875` "Smooth Motion - Enabled APIs"** — bitfield `1`=DX12, `2`=DX11, `4`=Vulkan,
  default `7`. Mask out only the API the game uses and keep Smooth Motion for everything else.
- `0xB0D384C0` "Smooth Motion - Enable" — `0`/`1`, per-app.
- **`0xB01B8B02` "Smooth Motion - Debug Bars"** — draws coloured bars on generated frames. This is
  the diagnostic that shows whether the flicker in #10 is "every other frame is wrong".

Post this to #1 and #10 now, with the mechanism, and ask both reporters for the Step 2 data.

### Step 2 — cheap A/B tests that discriminate the hypotheses

1. **`GetModuleHandleW(L"NvPresent64.dll")`** — have reporters confirm it is loaded in the game
   (Process Explorer / Resource Monitor). This is RTSS's own detection trigger.
2. **A D3D12 game with Smooth Motion on** (*LOTR: War in the North – Legacy Edition* is the
   known-good D3D12 row, `README.md:89`).
   D3D12 is the one path with no cross-device transport — NGX runs on the game's own device and
   queue. If it survives while D3D11/Vulkan fail, the transport is implicated too.
3. **Debug Bars on** — do the bad frames correlate with generated frames?
4. Confirm `[RenoDX.DLSS5] EnableHooks=2` in `ReShade.ini` (the feeder only writes it when a
   v45+/v4.6 add-on build is detected, `:230-231`; on a classic build the key does not exist and
   is irrelevant), and attach `dlss5-feed.log`. The lines that matter: `stopped: …`, `feature
   ready:`, whether `frame N delivered` keeps advancing — note it logs only the first
   `log_frames` frames and every 1800th (`:3655`), so ask reporters to set `log_frames` higher —
   and the 600-frame timing line from `TimingTick` (`:2593`).

### Step 3 — make the feeder thread-safe (the actual fix)

In `src/dlss5-feed.cpp`:

- **One re-entrant guard + lock around `FeedFrame`** (`:3683`). A `CRITICAL_SECTION` in the style of
  `g_log_cs` (`:74`, initialised in `DllMain` at `:4000`), taken for the whole of `FeedFrame` so the
  `g` struct, the allocator ring (`g.frame_slot`, `g.alloc_fence`, `:897-926`) and the shared
  textures cannot be touched by two Present threads at once. `FeedFrame` is the **single dispatch
  point** for all four backends (`FeedFrame11/12/Vk/Gl`, `:3688-3691`), so this one lock covers
  D3D11, D3D12, Vulkan (#10's path) and OpenGL — no per-backend guards needed.
- **`ID3D11Multithread::SetMultithreadProtected(TRUE)`** on the game's immediate context, queried
  once in `InitSession` (`:1511`, which already holds the `ID3D11DeviceContext` — and already
  queries `ID3D11DeviceContext4` for `g.ctx4` at `:1602`). Restore the previous value on teardown.
  Without this, `BlitOutputToBackbuffer` (`:2525`) — which does a save/restore of a *partial*
  device state (slot-0 SRV/sampler, one RTV/DSV, VS/PS, IA/OM/RS state, one viewport; no constant
  buffers or other slots, `:2528-2586`) — is unsafe the moment a second thread touches the context.
- **Record and log the calling thread id** on the first few frames, in the style of the existing
  `GetCurrentThreadId()` logging (`:2148`, `:2157` — currently GL-init only). If it changes
  mid-run, that alone confirms the off-thread Present and is the single most valuable line for
  diagnosing this class of report.
- The 32-bit add-on does **not** go through this code — it has its own `FeedFrame`
  (`src/dlss5-feed32.cpp:1971`) and its own `g_log_cs` (`:67`). Mirror the lock there.
  Smooth Motion's interposer is injected into the *game* process, not the host, so with the
  32-bit add-on's `FeedFrame` serialized the host's IPC message stream stays serial too.

### Step 4 — stop the silent, permanent stop (do regardless; cheap, low-risk)

- **`BeginCommands` (`:897-914`)** — split the fence timeout from the latch. On timeout, log and
  fail the *frame* (`return false` → the caller's `FeedFail`, `:886`) instead of calling
  `FeedDisable` directly; let the existing 3-consecutive-failure rule decide, with
  `consecutive_fails` already reset on every delivered frame (`:3654` on D3D11; each backend has
  its own reset). The host's `BeginCommands` (`host/dlss5-feed-host64.cpp:284-296`) **already
  behaves this way** — log and return false, no latch — so this aligns the add-on with it.
  Accepted cost: a genuinely hung GPU now stalls ~2 s per frame for 3 frames (~6 s) before
  "repeated failures" latches, instead of latching after one.
- Add cfg key **`gpu_timeout_ms`** (default `2000`) so a contended machine can raise it; add a row
  to the config table at `README.md:488-501`.
- **Surface the disable reason on the overlay.** The overlay already has the pattern — `g_mv_problem`
  via `ImGui::TextColored` (`:3952-3953`). Store `FeedDisable`'s `why` and show it beside
  `Session: disabled` (`:3887`), next to the existing **Re-enable** button (`:3898`).

### Step 5 — detection and honest docs

- **`DetectSmoothMotion()`** — `GetModuleHandleW(L"NvPresent64.dll")`, called from `DllMain`
  alongside `DetectRenodxAddon()` / `DetectToolkitAddon()` (`:4017-4018`), plus a re-check in
  `OnInitEffectRuntime` (`:3785`) since the module may load after the add-on. Log it, and show an
  orange overlay line in the exact style of the Alex's Toolkit warning (`:3892-3895`), naming the
  Profile Inspector per-API workaround from Step 1.
  Note: unlike `DetectToolkitAddon` (`:286`), which scans a *file* because ReShade may not have
  loaded the add-on yet, this one must be a **loaded-module** check — hence the re-check.
- **`README.md:39`** — replace the bare `##` heading with a proper blockquote in the style of the
  two existing ones (`:1-19`, `:21-37`): what Smooth Motion actually does (`NvPresent64.dll`,
  swapchain wrapper, off-thread multi-Present), the per-API Profile Inspector workaround, and the
  current status. Add a **Common cases** bullet (`:533-565`) and a TOC entry (`:59-81`).

### Deferred, only if Steps 3–4 do not fix it

A `sync_mode` cfg key (`0` = today's GPU-side wait, `1` = CPU-side wait): after `EndCommands()`,
block on `g.fence12` via `SetEventOnCompletion` + `WaitForSingleObject` (pattern already at
`:1476-1483`) then blit, instead of `g.ctx4->Wait(g.fence11, v_out)` (`:3651`). This removes the
cross-device GPU-side dependency the feeder injects into the game's immediate context right before
Present, at the cost of a CPU stall per frame. Only worth it if the pacer thread turns out to
disturb queue scheduling as well.

## Files

- `src/dlss5-feed.cpp` — `FeedFrame` (`:3683`) and `FeedFrame11/12/Vk/Gl`, `BeginCommands` (`:897`),
  `FeedDisable` (`:879`), `InitSession` (`:1511`), `BlitOutputToBackbuffer` (`:2525`), `Cfg` struct
  (`:368`) + `CfgReload` (`:428`), `DrawOverlay` (`:3879`), `DllMain` (`:3994-4044`)
- `src/dlss5-feed32.cpp` — its own `FeedFrame` (`:1971`) / `OnRenderTechnique` (`:2216`): mirror
  the lock, the cfg keys and the detection. `host/dlss5-feed-host64.cpp` — its `BeginCommands`
  (`:284`) already fails the frame instead of latching; nothing to change unless the cfg key is
  shared.
- `README.md` — `:39`, config table `:488-501`, troubleshooting `:533`, TOC `:59`

## Verification

- `build.bat`, `build-addon32.bat` and `host\build-host.bat` still link. (CI proves compile only —
  DLSS needs an RTX GPU and a real swapchain.)
- **Known-good regression check**, Smooth Motion *off*, Metro 2033 Redux (64-bit D3D11) and DOOM
  2016 (Vulkan): `frame N delivered` still advances and the 600-frame `TimingTick` line shows no
  new CPU cost from the lock.
- **Smooth Motion on**, same games: the new thread-id log line shows whether Present is off-thread;
  confirm the feed no longer latches off permanently (Step 4) and whether the corruption/flicker is
  gone (Step 3).
- Ship a test build to both reporters — #10 is the Vulkan data point that #1 cannot provide — and
  report back on both issues.
