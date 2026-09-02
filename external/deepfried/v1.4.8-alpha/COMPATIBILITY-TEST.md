# Deep Fried Chicken 1.4.8 alpha compatibility test

This build keeps the known-good Starfield baseline while correcting the
non-DLAA output domain and making common setup failures visible. It is still
an alpha: close the game before replacing files and keep a copy of your last
known-good build for rollback.

## Clean test setup

1. Remove or rename old Chicken/Alex Toolkit copies and the competing neural
   providers `renodx-dlss5.addon64` and `renodx-dlss.addon64`. A separate
   game-specific RenoDX HDR add-on may remain.
2. Copy `deep-fried-chicken.addon64`, `deep-fried-chicken-nvngx.dll`, and
   `deep-fried-chicken.cfg` beside the real game executable and ReShade proxy.
3. Supply your own trusted, signature-verified `nvngx_dlssnr.dll` in that same
   folder. NVIDIA's DLL is not in this archive.
4. For a D3D11 game that already supports native DLSS, also copy
   `dlss5-dx11-bridge.addon64`.
5. Launch once, open **Deep Fried Chicken > Compatibility / Startup**, enable
   early load if needed, confirm **Arm feature-1 interception on startup** is
   enabled, fully close the game, and relaunch. This saves restart-only
   `arm=1`; the live Enabled switch does not replace it.
6. Begin with one pass, 100% neural work scale, Texture Boost off, and Preserve Native Tone & Color
   off. Use the golden Pass 1 defaults: NR Preset/Style `Default`, all four
   strength sliders `2.0`, Automatic Mask and UI Correction off, game NGX
   depth convention, and Motion Scale X/Y `1.0`. Enable the game's own DLSS
   DLAA mode for the first baseline.

## Required checks

### Starfield golden-path regression

- 3840x2160, DLAA, RTX 4080 Super if matching the known baseline.
- Confirm one pass looks and performs like the preserved known-good build
  before trying more passes.
- In one-pass mode, change one harmless Pass 1 value and confirm the log/UI
  reports it as applied, then restore the golden value. Pass 1 must no longer
  bypass its controls.
- Toggle Preserve Native Tone & Color both ways and confirm the frame remains
  full-screen.
- Confirm the tab's `Last valid` line reports the detected native mode and the
  actual input-to-output resolution, labelled `automatic output sizing`.

### Live DLSS mode and dynamic-resolution matrix

- At one fixed output resolution, test all six native quality values:
  Performance, Balanced, Quality, Ultra Performance, Ultra Quality, and DLAA.
  Do not restart between modes.
- For every switch, confirm the native frame remains full-screen, neural
  evaluation resumes, and the tab updates its last-valid mode and detected
  input-to-output dimensions.
- A same-output mode or dynamic-resolution work-extent change should log
  `standalone adopted live game DLSS guide contract` and perform one history
  reset. It must not recreate feature 18 merely because the input extent or
  quality enum changed.
- Repeat several same-output work-extent changes if the game exposes dynamic
  resolution. Missing or inconsistent guide metadata must fail closed to the
  native frame rather than stretch or crop an old neural surface.

### Neural Work Scale matrix

- At one fixed output resolution and one pass, test 10, 25, 33, 50, 67, 99,
  100, 101, 125, and 150%. The displayed game resolution and HUD placement
  must never change. Exactly 100% must match the established native-size path.
- Confirm each applied value persists as `neural_work_percent`. The companion
  `neural_work_divisor` must remain a valid 1, 2, or 4 rollback hint. A config
  with only legacy divisor 1, 2, or 4 must load as 100, 50, or 25%.
- Test at least one odd display extent and 4K at 150% (5760x3240 working
  extent). If dimensions or the codec budget are unsupported, Chicken must log
  the rejection and leave the native frame intact rather than crop or crash.

### Smart multi-host discovery and ownership

- Run once with the ordinary game NGX host, then repeat with a title or helper
  that loads a second valid NGX owner later. The log should report canonical
  smart-hook owner groups without treating forwarder aliases as new owners.
- Confirm a created feature remains keyed to its exact hook slot and handle;
  an Evaluate or Release observed through another owner must not adopt it.
- Confirm startup logs `process-lifetime DLL notifications registered`. After
  the first successful native neural evaluation, expect `smart discovery
  settled: periodic full-module fallback disabled`. Run beyond 600 Presents
  and confirm the old 300-Present steady-state full-module scan does not recur.
- Load a valid second host after settlement. Its DLL-load notification must
  cause discovery on the later safe callback; the notification callback itself
  must do no enumeration, logging, or Detours work while under the loader lock.
- If notification registration is deliberately unavailable, confirm the
  300-Present fallback remains during bootstrap. After one native neural path
  succeeds, expect the log to reduce it to once per 3600 presented frames.
- Press **Refresh neural contract** after settlement and confirm it forces one
  discovery pass as well as the fenced graph rebuild.
- A renamed, weak export match is accepted only when it is unique; ambiguity
  must leave native output untouched.
- Resolver observation must return the genuine `GetProcAddress` address to the
  caller. Nested base/Evaluate-C implementations may reach the genuine target
  twice internally, but Chicken post-processing must run only once for the
  outer evaluation.
- On a module-heavy affected host, A/B the same scene and frame cap against the
  previous build. Record frametimes or 1% lows for at least two minutes; removal
  of the exact recurring scan is proven by logs/tests, but the reported hitch
  improvement still requires affected-host confirmation.

### Restart-only ownership gate

- Set `arm=0`, fully restart, and confirm the exported feature-1 interception
  state is DISARMED. Chicken must not claim its PID-suffixed consumer mutex and
  must install no native-NGX, Streamline, loader, or resolver hooks.
- Restore `arm=1` and fully restart. Confirm Chicken claims
  `Local\DeepFriedChicken.Feature1Consumer.v1.<decimal process ID>` before
  interception. Starting a second cooperating consumer first must report a
  conflict and leave native output active.
- Toggling the live Enabled checkbox must not change `arm` or pretend permanent
  hooks were removed; it should only drain/recreate Chicken-owned neural work.

### Frame Generation coexistence

Establish and record a stable FG-off run first. Frame Generation remains an
experimental, separate compatibility test; a passing DLSS/NR matrix is not
evidence that FG is safe in the same title.

- With a game that exposes Frame Generation, leave the coexistence experiment
  off and toggle FG on. NGX feature 11 must remain game-owned, the tab must say
  `native-safe bypass`, and native DLSS/DLSS-G must keep presenting without
  Chicken work or a black screen.
- Confirm the exact Streamline boundary is acquired before testing. Exercise
  both dynamic resolver retrieval and a run where Streamline cached its
  resolver/setter/tag pointer before Chicken loaded. The dynamic route should log
  `Streamline DLSS-G option boundary wrapped`; the pre-cached fallback should
  log `Streamline process-lifetime detour installed`; tag attachment should log
  `Streamline ResourceTag detours installed`. A topology change should log
  `applied without a blocking Present drain` with `native-safe-bypass`.
- No `Frame Generation pre-Present drain complete` or `pre-destruction drain`
  should appear in the new route. Seeing either means legacy blocking topology
  code was entered and is a failure.
- Toggle FG off. The log should report that FG Off reached one successful final
  Present, and Chicken should resume on the following valid native DLSS frame
  with reset history.
- Enable **Experimental: cook base frames with Frame Generation** only after the
  safe path passes. Chicken may run private work only when it can copy the
  result back into the native DLSS base frame on the same producer command list
  before that list closes. Expect repeated `inline base-frame copyback
  committed` evidence. Unsupported list ownership, resource state, or copy
  contracts must retain native output rather than guessing.
- While FG is on, switch DLAA -> Quality -> Performance -> DLAA. With the safe
  default, native DLSS-G must keep presenting while Chicken remains paused.
  The experiment is not expected to process generated frames.
- This matrix is required validation for 1.4.8. Multi-frame generation,
  parallel queues, resize, multiple viewports, and HDR UI recomposition remain
  WIP and need explicit per-title results.

### Disable, re-enable, and manual refresh lifecycle

- Toggle the main Enabled switch off and back on five times. After each off,
  the game must immediately show native output, then log a queued fence and a
  completed Chicken-owned graph teardown. After each on, the graph should be
  recreated lazily from the next valid contract without carrying prior
  sluggishness or temporal history. A no-fence completion is acceptable only
  when the log confirms that no Chicken GPU submission needed draining.
- Press **Refresh neural contract** once while Enabled. It should perform the
  same native-only fenced Chicken rebuild and then return to the last valid
  mode/resolution without restarting the game.
- Expected log fragments are `neural graph teardown fence queued` and
  `neural graph teardown complete after`. A clean run must not report queue
  tracking unreliability, uncertain NGX release, or a blocked teardown.

### Simplified controls / schema-6 and historical migration

- Set Neural passes to 30 and confirm every visible Pass 1 through Pass 30
  panel has the same NR Preset,
  NR Style, four strength sliders, Automatic Mask, UI Correction, depth
  convention, independent Motion Scale X/Y, and reset/retry button.
- Confirm Material Lab and Environment Detail controls are absent. Loading a
  schema-1, schema-2, or schema-3 preset containing their old keys must not
  reactivate either path.
- Load one preserved legacy preset. Supported values should be clamped and
  applied between wrapped calls; missing controls should receive golden
  defaults. Verify the source file is unchanged, then export and confirm the
  new file declares `preset_schema=6`, includes a complete Pass 30 record and
  all three Clean Fry values, and
  leaves the legacy source unchanged. A ten-record schema-4 preset must retain
  its first ten records while passes 11 through 30 receive golden defaults.
- Load a complete schema-5 preset and confirm every established setting is
  preserved while Clean Fry starts disabled with Cleanup Strength `0.65` and
  Detail Retention `0.85`.
- Remove one required Pass 30 or Clean Fry value from a schema-6 copy and load it. Confirm the
  entire preset is rejected and no live control changes.
- Change Scene Paper-White Scale, HDR Transfer Strength, and Color Strength one
  at a time. They must appear once in the shared Control-compatible section and
  affect the chain boundary, not multiply once per pass.
- Press a pass reset/retry button and confirm a safe between-call temporal
  reset/retry is logged. Do not expect a fatal device/contract latch to recover
  without native DLSS recreation or restart.

### Clean Fry final cleanup

- With one pass selected, enable Clean Fry and move both sliders. The UI must
  report a dry bypass and output must remain on the exact established decode
  path.
- With three passes selected, compare Clean Fry off/on at Cleanup Strength
  `0.35`, then `0.65`, with Detail Retention `0.85`. Check high-contrast edges,
  fine text, hair, foliage, skin pores, and slow camera motion for reduced
  ringing or colour buildup without obvious blur.
- Toggle Clean Fry and change both sliders while the scene is running. No
  neural feature recreation or temporal-history reset should be logged.
- Test SDR, HDR, Native Look off/on, Full/Half neural work scale, and at least
  one ultrawide resolution. Nonfinite shader math must fall back to the proxy;
  the output must not clip HDR to SDR range or alter native alpha.
- If the optional cleanup shader cannot compile on a system, the log should
  report the fallback once and the established final decode must continue.

### HDR carrier and neural work scale

- Test the same scene in SDR and HDR. With the NGX HDR flag set, a validated
  floating-point output carrier may use the linear-HDR codec; a UNORM or
  PQ-compatible carrier must stay on the non-linear-safe path. Neither case
  should become washed out merely because the flag is present.
- Compare Full and Half neural work scale at the same display resolution. Half
  should report a private work extent of `ceil(display/2)` while the final
  frame and UI remain at the unchanged display extent.
- Changing work scale must immediately use native output, fence and reclaim
  only Chicken-owned graph objects, then recreate safely. It must not release
  the game's feature-1, feature-11, or Feeder-owned handle.

### Grounded 2 / non-DLAA resolution fix

- Re-test the mode that previously rendered a small 3D scene in the upper-left
  while the HUD stayed full-size.
- At 2560x1440 Performance, the log should report a game contract similar to
  `1280x720->2560x1440` and a neural contract of
  `2560x1440->2560x1440`.
- Test one pass, then two passes, with Preserve Native Tone & Color both off
  and on. The 3D scene must fill the complete output in every case.
- Repeat at an ultrawide mode such as 3440x1440. A Quality-style work extent
  may be approximately `2293x960`, but both the neural Color/Output contract
  and final decoded scene must remain `3440x1440`.
- Enter and leave a menu which changes only the render/work extent several
  times. Each valid same-output contract should be adopted live with one
  history reset. A transient invalid contract should show the native frame and
  recover when a valid contract returns; it must not latch neural rendering off.
- Change the actual output from 3840x2160 to 2560x1440 and back to 3840x2160.
  Each output change must immediately use the native frame, fence and reclaim
  only Chicken's graph, then recreate at the new output without a game restart.
  The tab's detected resolution must follow both transitions. It must never
  composite a stale smaller surface or release the game's native DLSS/FG
  feature.
- The neural output allocation must have base `(0,0)`, exactly match the active
  output dimensions, and support unordered access. A non-zero output subrect,
  oversized output allocation, linked-node device, or another unvalidated
  resource layout must remain native-only. Passing the resolution matrix is
  not a claim of universal resource-layout support.
- Texture Boost is a separate exact-3840x2160-to-8K experiment. Leave it off
  for this matrix; enabling it at any other output resolution must leave the
  normal full-frame chain intact rather than scaling or cropping the scene.

### Hogwarts Legacy / provider and RR isolation

- Remove `renodx-dlss5.addon64`; do not merely untick its UI.
- Turn Ray Reconstruction off for the Chicken test and select native DLSS
  Quality or DLAA. Chicken 1.4.8 attaches to feature 1, not feature 13.
- With RR on, the tab should explicitly say Ray Reconstruction was detected and
  that native output remains untouched. This is diagnostic, not RR support.

### D3D11 / ESO-style startup test

- Use the included bridge only in a D3D11 game with native DLSS.
- Confirm early load is configured, enter the real menu or gameplay, and wait
  at least 60 seconds. A log ending on an 8x8 probe swapchain is too early to
  prove a game-render failure.

### DLSS5-Feeder transport test

- Use one official, version-matched Feeder package. Keep
  `dlss5-feed.addon64`/`addon32`, remove `renodx-dlss5.addon64`, and do not add
  the native-DLSS DX11 bridge.
- Feeder's upstream guide normally includes the Reno neural provider. Chicken
  replaces that one component for this test. First launch once with both files
  present and confirm the tab gives a specific Feeder-plus-Reno conflict
  message without attempting neural work; then remove Reno and restart.
- Set `warmup_rebuild=0` in `dlss5-feed.cfg`; begin at 100% work resolution.
- In a 32-bit game, put the three Chicken files and NVIDIA x64 DLLs in
  `host64`, not beside the x86 executable.
- Confirm Chicken logs the Feeder module or host and reaches a synthetic 1:1
  game/neural contract. After 600 frames, collect Feeder's MV/depth probes.
- For an ABI-aware Feeder build, confirm all four unsigned marker keys are
  published immediately before Create and every Evaluate:
  `DFC.Feeder.ContractVersion=1`,
  `DFC.Feeder.ProviderId=0x444C3546`, `DFC.Feeder.HostMode=0` (or `1` in the
  x64 companion), and `DFC.Feeder.EvaluateCadence=1`. The same complete tuple
  must remain bound to the created handle and hook slot.
- Negative tests: omit one marker, use an unknown value, or change the tuple at
  Evaluate. Chicken must reject only its added work and still call the genuine
  producer/native target. Confirm no more than one Chicken synthetic neural
  evaluation runs per Present and that release/recreate cycles reuse bounded
  Chicken slots without releasing the Feeder feature-1 handle.
- Follow `FEEDER-COMPATIBILITY.md` for Vulkan/OpenGL/D3D9 layouts and the
  upstream beta limitations. Source-contract compatibility is not a claim
  that every listed route has completed an in-game validation run.

### Memory / resolution-churn test

- Run for 10 minutes at one pass and record dedicated GPU memory at launch,
  minute 5, and minute 10. During the run, exercise all six same-output modes,
  FG off/on where available, five disable/re-enable cycles, and one
  3840x2160 -> 2560x1440 -> 3840x2160 output round trip. Memory may rise while
  a new graph/model is created, but it must settle after each fenced teardown
  rather than grow once per mode switch, toggle, or frame.
- Within one active graph the codec-contract count must never exceed four and
  codec-owned committed allocations must never exceed 1 GiB. At either limit,
  an unsupported new contract stays on native DLSS. A successful disable,
  manual refresh, or output rebuild should log that DFC handles, pair slots,
  codecs, and retained resources were reclaimed.
- When the game releases a native DLSS feature, the log reports the matching
  feature-18 release result and cumulative success/failure counters.
- Lines prefixed `[DLSS 5 Neural Rendering]` belong to the separate RenoDX
  provider, not Chicken. Do not diagnose those allocation lines as a Chicken
  leak unless a clean run without that competing provider reproduces it.

### Expected 1.4.8 log evidence

- Same-output mode/DRS switch:
  `standalone adopted live game DLSS guide contract`.
- Disable, refresh, or output/device rebuild with submitted Chicken work:
  `neural graph teardown fence queued` followed by
  `neural graph teardown complete after`.
- Frame Generation transition: dynamic acquisition should log `Streamline
  DLSS-G option boundary wrapped`; a pre-cached entry point should log
  `Streamline process-lifetime detour installed`, and tag detours should report
  installation. A successful change logs `applied without a blocking Present
  drain` and `native-safe-bypass`. Native feature 11 remains game-owned.
- If experimental base-frame coexistence is enabled, expect `color-tag handoff
  observed` where the title publishes that boundary, followed by `inline
  base-frame copyback committed` when the same-producer-list path succeeds.
  Until then, or on an unsupported copy contract, the tab/log must report
  native-safe bypass. The original tag calls, arrays, results, and generated
  frames must remain unchanged.
- Smart discovery should log either `periodic full-module fallback disabled`
  after DLL notification registration, or the conservative `reduced to once
  per 3600 presented frames` fallback after neural proof when registration is
  unavailable. Manual Refresh must request discovery without restarting the
  recurring 300-Present steady-state cadence.
- Treat `neural graph teardown blocked`, `queue tracking became unreliable`,
  `uncertain NGX release`, repeated feature-18 creation for same-output mode
  switches, or memory increasing every frame as failures and include the full
  log in the report.

## What to send with a report

- Exact symptom and when it starts.
- Screenshot of the entire Chicken tab including its Status line.
- `deep-fried-chicken.log`, matching `ReShade.log`, and for D3D11
  `dlss5-dx11-bridge.log`.
- For Feeder, `dlss5-feed.log` and the helper's `host64\ReShade.log` when
  applicable.
- `deep-fried-chicken.cfg` and only the `[ADDON]` section of `ReShade.ini`.
- Game/executable, API, resolution, native DLSS mode, GPU, driver, and ReShade
  version.

Do not send or redistribute your NVIDIA DLL. To roll back, fully close the
game and restore the three files from your preserved known-good package plus
the pre-upgrade config. Historical import is non-destructive: legacy schema-1
through schema-5 files are retained, and only an explicit export creates a
schema-6 preset.
