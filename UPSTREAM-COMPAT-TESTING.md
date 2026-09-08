# CK3 upstream compatibility testing

Branch: `testing/ck3-upstream-compat`, based on CK3 `5ede14d`.
This is a selective source port, with no upstream merge or change to the fork relationship.
The feeder identifies itself as `ck3-upstream-test.1` in ReShade and its log.

## Changes and provenance

Adapted from [DLSS5-Feeder](https://github.com/jlrouzies-fr/DLSS5-Feeder), under the repository's MIT license:

- [671aa4e](https://github.com/jlrouzies-fr/DLSS5-Feeder/commit/671aa4e): preserve BGRA output where typed UAV stores are supported; copy matching texel layouts home without applying a second sRGB conversion. Devices without BGRA UAV stores retain the converting fallback and log that limitation.
- [5347efc](https://github.com/jlrouzies-fr/DLSS5-Feeder/commit/5347efc) and subsequent v4.7 detection: identify newer RenoDX engines; default `NeuralUplift=1` and `NREnableUpscaling=0` only when unset. Existing hook policy and user overrides are preserved. The package continues to normalize its consumer filename to `renodx-dlss5.addon64`.
- [f17c396](https://github.com/jlrouzies-fr/DLSS5-Feeder/commit/f17c396): never execute a command list whose `Close()` failed. Cancel its readback, recreate the list, retain allocator/fence accounting, and suppress output copies in every affected caller. The upstream staleness-probe subsystem is not imported.
- [e4422df](https://github.com/jlrouzies-fr/DLSS5-Feeder/commit/e4422df): use the D3D12 allocation size for dedicated Vulkan imports. The optional physical-device/memory-type discovery is not ported because CK3's local layer and Streamline dispatch can bypass the loader hook.
- [20d7bef](https://github.com/jlrouzies-fr/DLSS5-Feeder/commit/20d7bef), with the dispatch reinstallation adjustment from [cc68576](https://github.com/jlrouzies-fr/DLSS5-Feeder/commit/cc68576): wait on and re-arm the game's present semaphores before an early Vulkan submit. Retire Vulkan copy-home work before destroying imports, and keep the original callback render target. No usable present context means mode 2 skips processing and logs the reason. The independent two-queue GPU regression is included.
- Small parts of [5582dbd](https://github.com/jlrouzies-fr/DLSS5-Feeder/commit/5582dbd) and cc68576: file-based mode changes request a feature rebuild; private-D3D12 queue waits are enqueued only after the command list opens. CK3 already had immediate overlay rebuilds and retains them.

The online installer now uses the same RenoDX **v4.55** archive and DLL SHA-256 pins as `Build-CK3-Complete-Test.ps1`. It rechecks cached files, rejects an unverified archive before extraction, and verifies the extracted add-on. Explicitly supplied/offline add-ons keep their existing validation rules and are not forcibly replaced.

The reviewed **310.8.0 + 310.8.SF-v2** Extended runtime pair is unchanged. Upstream's [driver/consumer findings](https://github.com/jlrouzies-fr/DLSS5-Feeder/issues/54) motivate the consumer pin; they do not establish CK3 compatibility. Newer supplied RenoDX engines produce a compatibility note in the log.

## Preserved CK3 scope

The custom DXGI bootstrap, package-local layers, four runtime profiles, Model L/M controls, and opt-in Native Streamline path remain. This branch does not adopt upstream's machine-wide installer, Deep Fried Chicken, DirectX/OpenGL/32-bit feature expansion, work-resolution controls, or its complete beta transport and crash-handling rewrite.

## Reproduce verification

From the repository root with VS 2022 Build Tools and the existing external SDK headers/libraries:

```cmd
build-local-current-tree.cmd
layer\build-layer-local.cmd
tests\test-feeder-compat.cmd
powershell -NoProfile -ExecutionPolicy Bypass -File tests\Test-CK3-RenoDxPolicy.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File tests\Test-CK3-Package.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File tests\Test-CK3-Builder.ps1
```

Verified on 2026-09-08:

- Feeder, Vulkan layer and DXGI bridge compile successfully.
- Actual feeder command submission on D3D12 WARP: rejected `Close()`, no false fence retirement/readback, subsequent valid submission, and a usable device. Format-layout and config round-trip regressions pass, including Model M and a file-based mode change.
- Vulkan ordering on NVIDIA GeForce RTX 3060: deferred submission **8 fresh**, ungated early flush **8 stale**, gated early flush **8 fresh**; final binary waits retire.
- Real pinned RenoDX download and hash verification; valid cache without network; poisoned-cache detection; archive rejection before extraction.
- Existing isolated package/profile and builder tests pass. These use fixtures and are not gameplay tests.

## CK3 testing still required

The refreshed feeder is staged at `ck3-package/binaries/dlss-payload/dlss5-feed.addon64`; its source build output is `build/dlss5-feed.addon64`. Use the branch's runtime-setup script together with that feeder, then rerun the selected profile installer so it refreshes `dlss-active`. Do not run the upstream generic installer over CK3.

Check DLSS45 Model M first, then Stock and Extended with the pinned consumer, then Native Streamline separately. Compare the same save, camera movement, UI and lighting against main. Check startup, profile switching, effect toggles/reloads, model changes, window/resize transitions and exit. Inspect colour/black levels, flicker/stale frames and NGX/NR evaluation logs; a visible overlay is not proof that neural rendering ran.

`vk_present_sync=1` is the default. For a controlled comparison only, set `vk_present_sync=0` in the active `dlss5-feed.cfg` to restore the previous submission ordering. A log saying `Vulkan mode 2 skipped: no usable present dependency context` means the new hook did not cover that launch/runtime combination; retain the logs before trying the diagnostic bypass. Shader output, colour conversion and the custom Streamline/bridge interaction still need in-game validation.