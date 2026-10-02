# CK3 upstream review — October 2, 2026

Selective updating is worthwhile. The strongest immediately usable improvements
are in the feeder shader: avoid an unused camera-fit calculation, stop feeding
partial motion vectors, and give the static-surface decision temporal memory.
These are now on `testing/ck3-upstream-compat`, together with NGX initialization
result logging and a rebuilt feeder. The subsequent `.3` refresh also carries
the Vulkan present-hook follow-up described below. CK3 gameplay gains remain
unmeasured.

## Reviewed state

- Fork: `skonester/CK3-DLSS`; production `main` at `5ede14d`.
- Starting test branch: `85fb1a9`, the September 8 selective port.
- Review window: September 2 through October 2, 2026, using the user's Chicago date.
- Fetched upstream: `jlrouzies-fr/DLSS5-Feeder`, `main` at
  [`7b41f5f3e57c260dec6012695c258e876fdfe6c5`](https://github.com/jlrouzies-fr/DLSS5-Feeder/commit/7b41f5f3e57c260dec6012695c258e876fdfe6c5), September 29,
  version **1.18.0-beta.1**.
- Latest fetched stable tag: `v1.17.0`, September 27, `03710dd`.
- Also inspected the separate `mgpu` branch, an older alpha consumer experiment
  with two commits absent from upstream main. It is not a newer stable baseline.

The 122 upstream commits absent from the starting test branch are not 122 missing
fixes: the branches diverged before September and our test branch adapted some
changes without retaining their original commit identities. The original
[compatibility notes](UPSTREAM-COMPAT-TESTING.md) remain the record of those ports.

A merge-tree simulation of the starting branch against upstream found conflicts
in the README, renamed/deleted plans, layer build script, both feeder translation
units, Vulkan imports and the present hook. It did not modify the checkout. A full
merge also introduces broad helper/API/consumer changes beyond our CK3 package.
The current refresh is a selective source port; it does not claim full upstream
merge ancestry or version 1.18 feature parity.

## Changes carried into this test branch

| Improvement | Source and concrete effect | CK3 implication |
|---|---|---|
| Skip unused camera fitting | [`7e3c3a3`](https://github.com/jlrouzies-fr/DLSS5-Feeder/commit/7e3c3a3), September 2: both fit shaders return immediately when `GEOM_ENABLE=false`. Previously a serial fit/solve ran every frame despite that option being off by default. | Removes unnecessary GPU work in the default shader path. The amount of frame-time improvement needs measurement. |
| Stable static decisions and complete motion vectors | Same commit: static rejection needs two consecutive wins; the first win keeps the provider vector and adds a current-frame mask. Rejected vectors are zeroed rather than scaled to arbitrary partial lengths. | Candidate reduction in shimmer/judder on slow map pans and flat surfaces. Adds two full-resolution R8 history textures, about 15.8 MiB total at 4K, plus bandwidth. `STATIC_HYSTERESIS=false` compares the temporal gate; the hard vector decision remains. |
| Consolidated guide pass and improved depth debug view | Shader as fetched at `7b41f5f`: raw depth is written alongside motion and mask instead of in a separate fullscreen pass; a named COLOR sampler supplies luma/debug reads. The debug depth display applies contrast without changing the depth supplied to NGX. | One fewer guide pass; depth orientation/offset handling and raw-depth contract are preserved. |
| NGX feature initialization result | Small part of [`012bb13`](https://github.com/jlrouzies-fr/DLSS5-Feeder/commit/012bb13), September 29: query and log `SuperSampling.FeatureInitResult` when reported, on D3D11, D3D12 and Vulkan sessions. | Makes `SuperSampling.Available=0` failures easier to diagnose. It does not change initialization or rendering decisions. |

The shader was taken from the pinned upstream tree with the D3D9-only stub removed
and the generic work-resolution comment adapted; the real effect body is otherwise
retained. The shader improvements were inside
the requested month but had not been included in our September 8 selective port.
The shader/diagnostic refresh reported `ck3-upstream-test.2`; the current feeder
with the subsequent Vulkan follow-up reports `ck3-upstream-test.3`. Its compiled payload is
staged at `ck3-package/binaries/dlss-payload/dlss5-feed.addon64`.
The package builder copies the refreshed shader from `shaders/DLSS5_Feed.fx`.

## Worth further CK3 work

| Priority | Upstream work | Assessment |
|---|---|---|
| Carried into `.3` | [`9febd0d`](https://github.com/jlrouzies-fr/DLSS5-Feeder/commit/9febd0d), September 16: preserve present context across loader/device hooks and install the device hook directly after creation. | Adapted with an early ReShade `init_device` hook for CK3 paths that bypass the loader create hook. Both CK3 local-layer and Native Streamline paths pass 32 actual presents each with the dependency gate active. Details below; gameplay remains a separate gate. |
| High | [`03b3f7b`](https://github.com/jlrouzies-fr/DLSS5-Feeder/commit/03b3f7b), September 27: distinguish newer RenoDX generations and update consumer acquisition. | Upstream reports helper tests of v6.1.0/v7.0.0-rc8/v8.0.1 on driver 617.14 passing 300/300 feature-18 evaluations, where v4.7 failed. That is useful evidence for a separate CK3 runtime matrix, not proof for our custom bridge, Extended pair or GPU. Do not silently replace our verified v4.55 archive/DLL pins with a moving latest download. |
| Medium | Same September 27 change: optional `feed_hold12.h` output stabilizer. | Potentially useful for changing neural output in otherwise still areas. It blends output based on input change and defaults off. The commit explicitly says the 64-bit integration had built but had not yet run in a 64-bit game. Test still views, animated portraits, HUD text and camera pans for lag/smearing before adopting. |
| Medium | [`0a9ed23`](https://github.com/jlrouzies-fr/DLSS5-Feeder/commit/0a9ed23), September 22: device-loss/DRED diagnostics and NGX fault attribution. | Better crash evidence would help support CK3. This spans substantial crash, transport and lifetime infrastructure; adapt separately with fault/recovery tests. |
| Low for CK3 | [`d2a7229`](https://github.com/jlrouzies-fr/DLSS5-Feeder/commit/d2a7229), September 29: run NGX in an external x64 helper. | Solves an in-process NGX problem demonstrated in shadPS4. It adds IPC and another process to our x64 Vulkan deployment, without a demonstrated CK3 need. |

Other expansion includes OptiScaler consumers, OpenGL/32-bit support, HDR10 bridges,
work-resolution/upscaling controls and a generic installer. None is automatically
a better CK3 architecture. The native-resolution Model M baseline, custom DXGI
bootstrap, local Vulkan layers, four profiles and Native Streamline opt-in remain
the scope of this branch.

Do not carry NGX log callbacks without a lifetime design. Upstream's latest beta
corrects its initial callback integration because NGX can retain the callback
after ReShade unloads an add-on. This port only reads a capability parameter.
Extra `settle_evals` are also not a performance improvement: upstream kept them
as a diagnostic after their flicker investigation, and each adds neural evaluations.

## Verification of the shader/diagnostic refresh on October 2

- Feeder build succeeds with the existing VS 2022 Build Tools and NGX SDK.
- Actual D3D12 WARP submission, failed `Close()` recovery, format-layout and config
  regressions pass (`tests/test-feeder-compat.cmd`).
- Existing NVIDIA RTX 3060 Vulkan two-queue test: deferred **8 fresh**, ungated
  early flush **8 stale**, gated early flush **8 fresh**; final binary waits retire.
- The real effect parses and assembles every entry point with ReShade **6.8.0**'s
  Vulkan SPIR-V backend, matching the package's pinned ReShade. All five provider
  modes pass at 1080p with normal depth/uniforms and 4K with reversed depth and
  performance specialization: **10 variants, 64 assembled entry points**.
- Isolated CK3 package/profile tests and builder tests pass. These use fixture
  DLLs and temporary game settings. Their ZIPs are test fixtures, not gameplay builds.
- Built/staged feeder SHA-256 matches:
  `5b586f33a010809cbe82ee73eb300a2ae1b00a0b40cd8d4c3007494dc1e35305`.

These checks establish build, effect compilation and existing transport/package
regressions. They do not establish CK3 image quality, FPS, driver pipeline
execution of the new shader, or actual DLSS 5 neural evaluation.

### Reproduce shader compilation

Prepare tooling in the ignored build directory from the repository root:

```powershell
git clone --depth 1 --branch v6.8.0 https://github.com/crosire/reshade.git build/compat-tests/reshade-source
# Expected source commit: 18deaa52de0c425a78b329e9cb3c497281cd00ec
git -C build/compat-tests/reshade-source submodule update --init --depth 1 deps/spirv
# Expected SPIRV-Headers commit: 7845730cab6ebbdeb621e7349b7dc1a59c3377be
Invoke-WebRequest -Uri 'https://raw.githubusercontent.com/crosire/reshade-shaders/6db142b4b1a05c764222e5b0bd9a644b7ccfe1dc/Shaders/ReShade.fxh' -OutFile build/compat-tests/ReShade.fxh
Get-FileHash build/compat-tests/ReShade.fxh -Algorithm SHA256
# Expected: 6dabfbbaf968c3871905d2ea17f96572ff7b1cec01310b5d0e5252b66b30174f
.\tests\test-ck3-shader.cmd
```

If tooling is already prepared, run the last command. The test compiles the actual
effect, emits per-entry-point SPIR-V files under `build/compat-tests`, and fails
on preprocessing, parsing or assembly errors. It does not load provider effects
or the game.

## Vulkan follow-up: `ck3-upstream-test.3`

The present-context provider now covers both the system loader's present export
and ReShade's layer/device dispatch. Nested wrappers for the same queue and
present info share one dependency gate; different nested presents restore the
caller's context through stack scopes. There is no fixed TLS nesting limit.
The missing-context path still skips full DLSS processing with synchronization
enabled; no automatic unsynchronized bypass was added.

The device hook installs after intercepted `vkCreateDevice`, and also at ReShade's
`init_device` event. The latter covers direct layer and Native Streamline startup.
A real test exposed that the system loader has not published the new device's
dispatch table at that event. The early hook therefore resolves through the
registered ReShade module's Vulkan layer entry point, with system-device dispatch
as fallback. Runtime initialization remains an idempotent retry.

ReShade shares its present entry point across devices. The feeder tracks live
Vulkan devices so destroying a temporary device does not unhook a surviving
device. Device destruction also removes hooks for sessions where feeding was
disabled, rather than depending on an NGX session having opened. Loader context
hooks are removed before the add-on unloads. Hook-install failures only remove
hooks created by this feeder, preserving another owner's hook.

Verification of `.3` on October 2:

- Feeder, local Vulkan layer and CK3 DXGI bridge compile successfully.
- The test observer includes the actual feeder translation unit and adds
  instrumentation only in the test DLL. ReShade 6.8.0 loads it through the real
  CK3 local layer chain; the test launches hidden fixture windows and actual
  swapchains on the RTX 3060.
- Both system and Native Streamline paths complete **32 presents / 32 covered /
  32 ordered**, with **2 devices hooked at initialization** each. The system
  fixture alternates loader-export and direct-device dispatch; the native fixture
  uses the CK3 shim's exported and device proc-address routes.
- Each path keeps the main device alive through eight temporary instance probes
  and through creation/destruction of a second device. Both survive without losing
  present coverage. The gate is called twice per frame to check idempotence; the
  final driver waits retire, and no context remains after returning to the caller.
- WARP submission/recovery and config regressions pass, including nested-context
  sharing/restoration assertions. The two-queue ordering test remains **8 fresh /
  8 stale / 8 fresh** for deferred / ungated / gated submission.
- The built/staged `.3` feeder SHA-256 matches:
  `a38737660b778dff163346c46dac04d9a7f559763253f4a02c7f0f61f56ff26e`.

Reproduce with VS 2022 Build Tools, existing SDK inputs, and the pinned ReShade
and Native Streamline payloads already acquired for a CK3 package:

```powershell
.\build-local-current-tree.cmd
.\tests\test-feeder-compat.cmd
powershell -NoProfile -ExecutionPolicy Bypass -File tests/Test-CK3-PresentHooks.ps1
```

`Test-CK3-PresentHooks.ps1` accepts `-GraphicsSource` for a directory containing
`ReShade64.dll`/`ReShade64.json` and `-NativeRuntimeDirectory` for the existing
Streamline DLL directory. Defaults point at the previously built all-profiles
package under `release`. It rebuilds the local layer/bridge and instrumented
test add-on, restores its process's Vulkan environment, and keeps fixture logs
under `build/compat-tests/present-hooks-system` and `present-hooks-native`.
Each fixture process has a 60-second timeout; no game files or global layer
registration are changed.

The fixtures intentionally disable feeder evaluation and load no feeder effect.
They establish real hook coverage and binary-semaphore ordering, not CK3 gameplay
or DLSS/NR evaluation. The existing Streamline payload's optional `NvLowLatencyVk`
module was rejected by its signature check; Streamline initialization and the
tested presentation routes still passed. No newer RenoDX/runtime pair or output
stabilizer was added in this follow-up.

## Gameplay gate before promoting to main

Build a fresh CK3 package with this feeder and the local CK3 layer/bridge. Installing
only the feeder would miss the shader improvements. Use the same save, resolution,
provider, driver and settings for both branches; compare a still map, slow pan,
fast pan/zoom, character portraits and UI text. Start with DLSS45 Model M. Then
check Stock/Extended with our pinned consumer and Native Streamline separately.

Record frame times with geometry fitting off, inspect shimmer and black levels,
and check reloads, profile switches, resize/window transitions and exit. Retain
ReShade/feeder/bridge logs and confirm feature evaluation, rather than relying on
an overlay being visible. Keep `vk_present_sync=1`; a missing-present-context log
still identifies a coverage gap; retain that launch's logs even though the fixture
paths now pass.

Promote this selective shader/Vulkan refresh only after that CK3 comparison.
Evaluate newer RenoDX/runtime pairs in their own experiment with immutable
versions and hashes; assess the output stabilizer separately with it defaulted off.
