# Plan: post-0.12.0 review fixes (DLSS5-Feeder)

## Context

A full code review of the 64-bit add-on (`src/dlss5-feed.cpp`), the 32-bit add-on
(`src/dlss5-feed32.cpp` + `feed_gl.h`/`feed_vk.h`), the host (`host/dlss5-feed-host64.cpp`) and the
shader (`shaders/DLSS5_Feed.fx`) was done on 2026-09-02 against `main` (0.12.0 merged). Every
finding below was re-verified by reading the code at HEAD `26298db` ("settle_evals", IPC v8); that
commit does not touch any of the areas changed here. Findings that did not survive verification are
listed at the end so nobody re-chases them.

Four phases, in order of value per effort:

- **Phase 0**: The Surge 2 (64-bit Vulkan) "flicker / constant judder" report from 2026-09-02
  evening: what the logs prove, the A/B the user runs first, and the three code fixes that fall
  out of the log regardless of the A/B result.
- **Phase 1**: small, verified bugs. One-to-ten-line fixes each. One branch, one commit per numbered item.
- **Phase 2**: stuck keys in the cast panel (32-bit). ~40 lines.
- **Phase 3**: the 32-bit render thread no longer blocks on the host (spawn, handshake, build ack,
  and a hung host). A restructuring of the 32-bit build path; its own change and its own game test.

## Ground rules for the implementer

- Source files are **UTF-8 with BOM + CRLF**. Use the Edit tool only. Never `sed -i` (it strips CRLF;
  if it happens, restore with `sed -i 's/\r$//; s/$/\r/'` before committing).
- Line numbers below are at HEAD `26298db` and are approximate. **Anchor on the function name and
  the quoted code**, not the number.
- Build: `cmd //c "G:\AI\DLSS5-Feeder\build.bat"` (64-bit add-on), `cmd //c "G:\AI\DLSS5-Feeder\build-addon32.bat"`,
  `cmd //c "G:\AI\DLSS5-Feeder\host\build-host.bat"`. Grep the output for `error|warning`. There is no
  in-repo test rig for the add-ons; the host has `dlss5-feed-host64.exe --test --hide` (300-frame
  self-test, must stay 300/300). Shader changes: compile offline with fxc is not possible for .fx;
  ReShade compiles it on load, so check `ReShade.log` in the test game for errors.
- Branch: `v0.12.1` off `main` (the repo's branches and tags share the release name). Do not bump
  `FEED_VERSION` / `src/version.rc`; that happens at release time.
- Deployment for testing follows `DEPLOY-DEV.md` (Fable Anniversary = 32-bit D3D11 client via
  dgVoodoo; Metro 2033 Redux = 64-bit D3D11; WormsXHD = 32-bit OpenGL; Castlevania LoS under DXVK =
  32-bit Vulkan). The user runs the games; the implementer only builds and deploys.
- Commit message trailer required:
  `Co-Authored-By: Claude ... <noreply@anthropic.com>` and `Claude-Session: <url>` as the harness instructs.

---

## Phase 0: The Surge 2 judder (64-bit, Vulkan)

Install: `E:\SteamLibrary\steamapps\common\The Surge 2\bin` (64-bit, Vulkan through the machine-wide
ReShade 6.8.0 layer, RTX 5090, driver 616.56, 3840x2160, display at 40 Hz, game at a locked 40 fps).
Consumers tried by the user: Deep Fried Chicken, RenoDX classic 0.2026.827 + Alex's Toolkit (1 and
2 pass), RenoDX alone. The judder is present in every combination, so it is not the consumer.

### What the logs prove (session 20:01-20:03, `dlss5-feed.log`, `ReShade.log`, `alexs-toolkit.log`)

- The deployed add-on is the 0.12.0 release build (built 17:38, before the settle_evals commit).
  `git diff v0.11.0-beta.2 v0.12.0 -- src/dlss5-feed.cpp` touches the Vulkan frame path and the
  fence/wait/copy-home code in **no line**; the SR/jitter work is `FeedFrame11` only. So this is
  not a 0.12.0 regression in the transport.
- The deployed `reshade-shaders\Shaders\DLSS5_Feed.fx` is byte-identical to the repo's (sha256
  `955d911d…`), i.e. the **validation shader from commit 2d6ae38 (2026-09-01, "Validate motion and
  depth guides (#18)")**, copied into that folder at 06:39 on 09-02. The preset enables
  `VALIDATE_STATIC=1, VALIDATE_DEPTH=1, VALIDATE_MV=1, MV_VALIDATE=1, VALIDATE_LUMA=0, MASK_STRENGTH=1`,
  provider LumeniteFX Kernel (`DLSS5_MV_PROVIDER=3`).
- Frame pacing is clean: 25.00 ms intervals, feed CPU 0.8 ms, 0-1 stalls per 600 frames outside
  loads. The judder is in the image, not the cadence.
- **Depth guide flat during real gameplay**: `Depth probe ... min 0.249981 max 0.249981 variance 3.66e-15`
  at frames 600, 1200, 1800, 2400, 3000 while the MV probe shows 2-37 px of motion at the same
  frames (frame 1800: mean 37 px). After the 4.5 s load at frame 3346 the depth becomes real
  (0.002-0.01). ReShade's Generic Depth was on the wrong buffer for the first three minutes. DLSS
  and the neural pass both take depth as a guide; a flat depth breaks their disocclusion handling.
- **Present-probe false positive**: "an external frame pacer is presenting this swapchain: 190
  presents against 121 frames fed (1.57x)" at frame 121, then the cumulative ratio decays 1.28x,
  1.19x … 1.01x with exactly 120 presents per 120 fed frames. The surplus is the ~68 presents
  between hook install (20:01:15) and the first fed frame (20:01:22, menu + shader compile).
  `src/feed_vk_hook.h:87-104` compares presents-since-hook against frames-fed. Diagnostic only
  (no behaviour depends on it), but the message sends users to Profile Inspector for nothing.
- **Crash at 20:03:58** when the user toggled ReShade's "Copy depth buffer before clear":
  `0xC0000005 in nvoglv64.dll; last doing: waiting for the result (Vulkan)`, logged by **eight
  threads at once**. `CrashFilter` (`src/dlss5-feed.cpp:154`) has no once-guard, `WriteCrashDump`
  opens the dump with share mode 0 (seven threads: error 32) and `LoadLibraryW(L"dbghelp.dll")`
  inside the filter was refused by ReShade ("Ignoring LoadLibrary('dbghelp.dll') call to avoid
  possible deadlock" x3); the one thread that got the file failed `MiniDumpWriteDump` with
  0x80070006. So there is no dump to read. The 32-bit `CrashFilter` (`dlss5-feed32.cpp:131`) has a
  plain `static int crashes` counter (not interlocked) and the same dbghelp load.

### Hypothesis (the user confirms or refutes with the A/B below)

The static-hypothesis test in `ValidateTests` (`DLSS5_Feed.fx:443-454`) decides per pixel per
frame, with no memory, whether "did not move" explains the patch better than the provider's vector;
when it wins, `PS_MotionVectors` zeroes the vector (`mv = flow * (1.0 - zero_vector)`, :707-709)
and does **not** raise the mask for it (`distrust` excludes `bad.w`). On low-contrast surfaces under
a slow pan, the decision flips between frames, so DLSS alternately reprojects and does not: exactly a
"flicker / judder that comes and goes" and independent of the consumer. The depth and consistency
tests (`bad.y`, `bad.z`) are fractional, so they also produce half-length vectors that point
nowhere. The flat depth makes everything downstream worse but is a ReShade depth-selection matter.

### A/B for the user (no build needed, in this order; each one is a live preset edit)

1. ReShade overlay, DLSS5_Feed.fx variables: `VALIDATE_STATIC` off. Judder gone? -> hypothesis confirmed.
2. If not: `MV_VALIDATE` off (the whole validation). Gone? -> the depth/consistency tests.
3. If not: enable `DLSS5_Feed_Debug` (`DEBUG_VIEW` depth) or DisplayDepth.fx during gameplay: is the
   depth flat? Then fix Generic Depth (pick the right buffer in the Add-ons tab) and re-check.
4. Only if 1-3 change nothing: swap `dlss5-feed.addon64.11.0.beta` back in (same folder) to
   exclude the add-on build itself.

### Code changes (0a and 0b are conditional on the A/B; 0c-0e are certain)

**0a. Shader: one-frame hysteresis for the static decision** (`shaders/DLSS5_Feed.fx`).
- New history texture `DLSS5_PrevStatic` (R8, screen size) with sampler; written by `PS_StoreHistory`
  as a fourth render target of the `History` pass: 1.0 when this frame's *validated* vector
  (`tex2Dfetch(sDLSS5_MV, ...)`, already written by the `Guides` pass) is zero while the raw
  provider vector is above 0.5 px, else 0.0 (i.e. "the static test won here this frame").
- In `PS_MotionVectors`'s `MV_VALIDATE` branch: `bad.w` may zero the vector only if
  `tex2Dlod(sDLSS5_PrevStatic, uv).x > 0.5` (won last frame too); otherwise keep `flow` and set
  `distrust = max(distrust, GEOM_MASK_REJECTED)` (reuse that 0.35 uniform, or add `STATIC_MASK`) so
  the first frame of a suspected static pixel is a "trust the current frame" hint rather than a
  reprojection flip. Reset the history on `reset` (it clears with the others).
**0b. Shader: no fractional vectors.** `zero_vector = max(bad.y, bad.z, bad.w) > 0.5 ? 1.0 : 0.0`
for the vector; keep the soft values for the mask only. (Was listed under "Not doing" before the
Surge log; the A/B decides.)
**0c. Pacer detector baseline** (`src/feed_vk_hook.h`): add `static LONG64 g_vk_presents_base;`
set in `FeedVkPresentTick` when `fed_frames == 1` (`g_vk_presents_base = g_vk_presents`), and
compare `presents - g_vk_presents_base` against `fed` in `FeedVkHookQueuePresent`, and report the
same difference in the periodic probe. Keep the `fed > 120` arming. Also make the warning say what
was measured ("N presents per M fed frames since the first fed frame").
**0d. Crash filter, both add-ons** (`src/dlss5-feed.cpp:134-168`, `src/dlss5-feed32.cpp:~110-149`):
- `static volatile LONG g_crash_once;` and at the top of `CrashFilter`:
  `if (InterlockedCompareExchange(&g_crash_once, 1, 0) != 0) return EXCEPTION_CONTINUE_SEARCH;`
  (other threads fall through to the game's handler immediately; keep the 32-bit "one dump per
  process" comment, it is the same rule).
- Resolve `MiniDumpWriteDump` early: a `static PFN_MiniDumpWriteDump_ g_write_dump;` filled from
  `LoadLibraryW(L"dbghelp.dll")` in `OnInitEffectRuntime` (64-bit ~:5636, next to
  `DetectStaleD3DCompiler()`, which the comment there says is the safe place for a LoadLibrary) and in
  the 32-bit equivalent. `WriteCrashDump` uses it and only falls back to `LoadLibraryW` if null.
- Open the dump file with `FILE_SHARE_READ` and log the `GetLastError()` of `MiniDumpWriteDump`
  before `CloseHandle` (the current code logs the error after `CloseHandle`, which overwrote it).
**0e. Log hint for the flat-depth case** (`src/dlss5-feed.cpp`): `MvProbeAnalyse` (~:1883) runs
right before `DepthProbeAnalyse` (~:1913) for the same captured frame. Keep the mean it computes in
a `static double g_mv_probe_mean_px;` and, in `DepthProbeAnalyse`, when `flat && g_mv_probe_mean_px > 1.0`
replace the suffix with: "depth is flat while the scene moves: ReShade's Generic Depth is on the
wrong buffer (Add-ons tab -> Generic Depth); DLSS and the neural pass get no depth until it is
fixed". Show the same text on the overlay's Depth line (`g_depth_probe` is what the overlay prints).

Open, not planned: the nvoglv64 crash on toggling "Copy depth buffer before clear" on Vulkan. With
0d in place the next occurrence produces a dump; chase it then.

---

## Phase 1: verified small fixes

### 1.1 64-bit: a resize on the same-device D3D12 path stops the feed forever (HIGH)

`src/dlss5-feed.cpp`, the four "grace gate" sites (`FeedFrame12` ~:4157, `FeedFrameVk` ~:4415,
`FeedFrameGl` ~:4987, `FeedFrame11` ~:5263), all shaped:

```cpp
const bool needs_build12 = !g.frame_ready || w != g.width || h != g.height || cd.Format != g.bb_fmt;
if (g.frame_ready && needs_build12) g.create_grace = 0;
if (ok && needs_build12 && g.create_grace < create_delay12) { if (++g.create_grace == 1) Log(...); ok = false; }
```

**Bug**: when `frame_ready` is true and the size changed, the first line resets the grace to 0 on
*every* frame before the second line increments it, so it never passes 1 and the build never runs.
Nothing clears `frame_ready` on the same-device D3D12 path (`OnDestroyEffectRuntime` deliberately
keeps the resources; `OnInitEffectRuntime` only clears it when `g.dev12_owned`). The log shows
"holding the feature (re)build" every frame and the feed is dead until the game restarts.

**Fix** (all four sites): make it a transition, i.e. `if (g.frame_ready && needs_buildXX) { g.create_grace = 0; g.frame_ready = false; }`.
Once `frame_ready` is false, `needs_build` stays true through `!g.frame_ready`, the grace counts up,
the build runs. Keep the log line. Optionally factor the three lines into a `static bool GraceGate(bool needs_build, int delay)`
helper used by all four; not required.

### 1.2 64-bit: overlay edits to creation-time settings never apply (HIGH)

`DrawOverlay` (~:5764-5935) writes into `g_cfg` directly and calls `CfgSave()` when `dirty`.
`CfgReload()` (~:1010) starts from `Cfg next = g_cfg`, parses the file, and only reports a rebuild
when `next` differs from `g_cfg`. After an overlay edit the file equals memory, so HDR, Depth
inverted, Preset, Raw create flags and "Force one rebuild" never trigger a rebuild. `mode` is also
missing from the rebuild set, so a mode change from the file never rebuilds either; mode 1 to 2 then
evaluates with a null feature (it fails and self-heals through `FeedFail`, but it costs a failure).

**Fix**:
1. In `DrawOverlay`, wherever one of these is changed, also request the rebuild:
   `mode` (~:5829), `hdr` (~:5876), `depth_inverted` (~:5877), `preset` (~:5896), `flags` (~:5930),
   the "Force one rebuild" button (~:5932). Add `g.frame_ready = false;` next to `dirty = true;` at
   those six sites (a bool store; the frame path picks it up at its next `needs_build`).
   Do NOT do it for reset_every, log_frames, mv_scale, work_sharpness, settle_evals, create_delay,
   warmup_rebuild (runtime-only values).
2. In `CfgReload`, add `next.mode != g_cfg.mode` to the `rebuild` expression (~:1063-1066).
3. In the Re-enable button (~:5818-5824) also reset `g.create_fail_count = 0;` and `g.create_grace = 0;`
   (`OnCreateFeatureFailed` ~:2429 disables at `>= 3` and the counter is only reset after a successful
   create, so a re-enable after three failures re-disables at the next attempt).
4. Belt and braces: in each of the four frame functions, in the non-transport evaluate branch
   (the `else` of the `if (g_cfg.mode == 1)` test at ~:4195 D3D12, ~:4529 Vk, ~:5029 GL,
   ~:5299 D3D11 where it reads `if (ok && g_cfg.mode == 1)`), add
   `if (g.feature == nullptr) { g.frame_ready = false; return; }` (D3D11: `ok = false;` instead of
   return if code after the branch must still run; read the function tail first) before `BeginCommands()`.

### 1.3 64-bit: D3D11 `InitSession` failure leaves a half-built session (MEDIUM)

`InitSession(ID3D11Device*, ID3D11DeviceContext*)` (~:2748), label `fail:` (~:2867):
```cpp
fail:
    if (adapter != nullptr) adapter->Release();
    FeedDisable("the D3D12/NGX session failed to start");
    return false;
```
`InitSession12/Vk/Gl` call `ShutdownSession()` on every failure; this one does not, so `g.dev12`,
the NGX init, `g.params`, queue/list/fences and the `ID3D11Multithread` change survive. Re-enable
then re-runs `InitSession`, leaking the first device and calling `NVSDK_NGX_D3D12_Init` twice.

**Fix**: add `ShutdownSession();` before `FeedDisable` at `fail:`. `ShutdownSession` (~:2873)
already restores `g.mt` (~:2890 `if (!g.mt_was_on) g.mt->SetMultithreadProtected(FALSE);`) and
null-checks every release, so it is safe on a half-initialised session; keep it so.

### 1.4 64-bit: `CfgSave` erases keys it does not know (MEDIUM)

`CfgSave` (~:1003) and `CfgWriteDefault` (~:888) write 22 keys. The parser also reads `half_home`,
`passthrough`, `jitter_sign`, `jitter_phases` (commented "parse-only, not written back"). Any
overlay change rewrites the file without them, so a user-set `jitter_sign=-1` (the README asks users
to try it) is silently lost at the next launch.

**Fix**: make `CfgSave` preserve unknown lines. Read the existing file first, keep every line whose
key is not one of the 22 it writes (and keep comments / blank lines), write the 22 known keys, then
append the preserved lines. Do the same in the 32-bit `CfgSave` (`src/dlss5-feed32.cpp` ~:361) for
symmetry even though it currently writes every key it parses. Keep the format `key=value\n`.

### 1.5 Both add-ons: the crash filter outlives the DLL (MEDIUM)

`DllMain` `DLL_PROCESS_ATTACH` installs `g_prev_filter = SetUnhandledExceptionFilter(&CrashFilter);`
(64-bit ~:5965, 32-bit ~:4067). `DLL_PROCESS_DETACH` (64-bit ~:5990, 32-bit ~:4083) never restores
it. ReShade unloads and reloads add-ons between Vulkan instances (see `feed_vk_hook.h` header
comment), so after an unload any crash jumps into unmapped memory and masks the real fault.

**Fix**: first statement of both detach blocks: `SetUnhandledExceptionFilter(g_prev_filter);`.
Neither file calls `DeleteCriticalSection` anywhere (verified by grep): add it for `g_feed_cs` and
`g_log_cs` as the very last statements of detach, after the final `Log(...)`.

### 1.6 Both add-ons: depth-reversed definition ignores the per-effect scope (LOW, cheap)

64-bit `ResolveHandles` ~:5539 and 32-bit ~:3591:
`if (rt->get_preprocessor_definition("RESHADE_DEPTH_INPUT_IS_REVERSED", v))` (global only), while
the provider lookup right above (`ReadMvProviderMode`, 64-bit ~:1124, 32-bit ~:533) tries
`get_preprocessor_definition_for_effect(kEffectFile, ...)` first. A user who set the depth
definition on `DLSS5_Feed.fx` only gets the shader running reversed while DLSS is told the opposite.

**Fix**: mirror the provider pattern:
`if (rt->get_preprocessor_definition_for_effect(kEffectFile, "RESHADE_DEPTH_INPUT_IS_REVERSED", v) || rt->get_preprocessor_definition("RESHADE_DEPTH_INPUT_IS_REVERSED", v))`.

### 1.7 64-bit: `ResolveHandles` reads ReShade.log on every call (LOW, perf)

`ResolveHandles` (~:5505) calls `ProviderCompileError` (~:1077, reads up to 512 KB of the log
tail and splits lines) at ~:5535, before the signature early-return at ~:5551. Runtimes can be
recreated in bursts (Smooth Motion, Space Engineers) and each call pays the file read on the render
thread.

**Fix**: inside `ProviderCompileError`, cache by file identity: `_stat` the log, and if size and
mtime equal the cached values, return the cached result. Keep the rest as is.

### 1.8 32-bit: "Re-enable" cannot recover after the host died on D3D11 (MEDIUM)

`HostClose` (~:813) ends with `if (g.is_gl || g.is_vulkan) g.built = false;` (D3D11 keeps `built`
so a host restart re-uses the game-created textures). `HostLost` = `HostClose` + `FeedDisable`.
Then Re-enable (`DrawOverlay` ~:3761) clears `g.disabled`, but `g.built` is still true and
`g.hproc` is null, so `FeedFrameDispatch` skips the build (~:3426 `if (ok && (!g.built || size_changed ...))`)
and hits `if (ok && g.built) { if (!HostAlive()) HostLost("process died"); }` (~:3440) on the very
next frame: disabled again. Only the "Restart the DLSS 5 host" button works.

**Fix**: in the Re-enable handler add `if (!HostAlive()) g.built = false;` (comment: a dead host means
new fences at least; the D3D11 textures are recreated cheaply by `BuildShared`).

### 1.9 32-bit: Vulkan device recreation leaves a dead panel image (MEDIUM)

`FeedFrameVk` (~:3103), block "A recreated device strands every import":
```cpp
for (int i = 0; i < FEED_SLOTS; ++i) { g.vk_img[i] = VK_NULL_HANDLE; g.vk_mem[i] = VK_NULL_HANDLE; }
g.vk = {};
...
HostClose(); ReleaseShared();
```
`g.vk = {}` runs before `ReleaseShared`, whose panel cleanup sits under `if (g.vk.ok)`, so
`g.vk_panel`, `g.vk_panel_mem`, `g.vk_panel_init` keep the old device's handles. Next build:
`CastImportPanelVk` returns early because `g.vk_panel != VK_NULL_HANDLE` (leaking the new handle),
and `CastVkDrawPanel` blits a VkImage of the destroyed device.

**Fix**: in that block, next to the `vk_img`/`vk_mem` loop, add
`g.vk_panel = VK_NULL_HANDLE; g.vk_panel_mem = VK_NULL_HANDLE; g.vk_panel_init = false;`.
The GL side needs nothing: its only teardown path (`OnDestroyDevice` ~:3680) calls `ReleaseShared`
while `g.gl.ok` is still true, and `ReleaseShared` (~:1896-1903) zeroes the GL panel fields there.

### 1.10 32-bit: handle leaks and the missing panel on the D3D11 host-creates path (LOW)

Applies to feature-level 10.x D3D11 games (issue #33, NFS MW 2012) where `g.host_creates` is set.

a. `BuildShared` (~:2127-2142): the 'B' exchange, then `if (!ack.ok) return false;` BEFORE the
   handles are captured at ~:2152 (`g.tex_handle[i] = ack.tex[i]`). The host duplicates all four
   handles (and the panel) into the game even when `CreateFeature` fails afterwards, so every failed
   build leaks five handles. `BuildSharedVk` ~:2502 has the correct pattern with a comment ("Take
   ownership of the duplicated handles NOW"). **Fix**: move the capture loop to right after the
   `PipeRead(&ack)` succeeds, before the `SR_UNAVAILABLE` recursion and the `!ack.ok` return, guarded
   by `if (g.host_creates)`. `ReleaseShared` (~:1919) already closes `g.tex_handle[]` on every path,
   and `BuildShared` calls `ReleaseShared()` first (~:2047), so captured handles are never leaked.
b. Same branch never reads `ack.panel_tex`; the host ignores `b.panel_tex` when it owns the panel
   (`h.panel_host_owned`). Result: texture cast shows a never-written texture and leaks a handle
   per build. **Fix**: in the `g.host_creates` branch, after the slot loop: if `ack.panel_tex != 0`,
   `CastReleasePanel()`-equivalent for the SRV/tex only (keep `panel_w/h`), then
   `dev1->OpenSharedResource1(handle, IID ID3D11Texture2D, &g.panel_tex)`, create the SRV exactly as
   `CastMakePanel` does (~:1009), store the handle in `g.panel_handle` so `CastReleasePanel` closes it.
   On any failure `CloseHandle` the duplicated handle.
c. `CastImportPanelGl`/`CastImportPanelVk` (~:1047-1069): the early return
   `if (ack.panel_tex == 0 || g.panel_w == 0 || g.gl_panel_tex != 0) return;` leaks the freshly
   duplicated handle whenever the panel is already imported (every rebuild after the first).
   **Fix**: if `ack.panel_tex != 0` and we are not importing, `CloseHandle` it before returning.

### 1.11 Host: five small robustness fixes (`host/dlss5-feed-host64.cpp`)

a. **Panel released with a copy in flight** (~:1808): `if (h.panel != nullptr && !h.panel_host_owned) { h.panel->Release(); ... }`
   on every D3D11-client build. `CopyPanel` (~:928) runs on `h.pump_queue` and signals `g_panel_fence`;
   the rebuild drain right above (~:1691) only waits `h.fence`. **Fix**: before the release,
   `if (g_panel_fence != nullptr) WaitFenceValue(g_panel_fence, g_panel_val, 500);` (same as
   `ShutdownDisguise` ~:2007).
b. **Rebuild drain skipped in transport mode** (~:1691): `if (h.feature != nullptr && !WaitFenceValue(h.fence, h.fence_value, 2000))`.
   In transport mode the feature is null but the last `CopyTextureRegion` may be in flight when
   `h.tex[]` are released. **Fix**: drain unconditionally: `if (!WaitFenceValue(h.fence, h.fence_value, 2000)) Log(...)`
   (it returns immediately when the queue is idle).
c. **`AbortCommands` can leave `h.list == nullptr`** (~:705-714) when `CreateCommandList` fails;
   `BeginCommands` (~:641) then dereferences it at `h.list->Reset(...)`. **Fix**: at the top of
   `BeginCommands`, `if (h.list == nullptr) { Log("[host] no command list (a previous NGX fault could not be recovered)"); return false; }`.
   The 'F' path already CPU-signals `fence_out` and checks `DeviceRemoved` when `Evaluate` fails.
d. **Init failures bypass the exit tail** (~:2101-2103): `if (!InitDisguise()) return 1;` and
   `if (!InitNgx()) { ...; return 1; }` skip `ShutdownDisguise()` and the `TerminateProcess` the
   comment at ~:2106 explains (ReShade's DLL teardown hung after the window went, WormsXHD).
   **Fix**: `int rc = 1; if (InitDisguise()) { if (InitNgx()) rc = test ? RunTest() : Serve(pid); else Log("[host] NGX unavailable"); }`
   then the existing `ShutdownDisguise(); Log("[host] exit %d", rc); TerminateProcess(...)`.
e. **D3D11-client textures opened unvalidated** (~:1793-1806): after `OpenSharedHandle` succeeds
   for slot `i`, `GetDesc()` and require: width/height == `b.width/b.height` for COLOR/DEPTH/MV and
   == `out_w/out_h` for OUTPUT; format == `b.color_fmt` for COLOR, `out_fmt` for OUTPUT; and
   `D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS` on OUTPUT unless `no_uav`. On mismatch log the
   slot, the expected and actual values, and set `ok = false` (a clean "build failed" ack instead of
   a DEVICE_HUNG later). The panel already has this check (~:1823); copy its shape.
f. **Unchecked fence duplication** (~:1627-1628): both `DuplicateHandle(... hin/hout -> hgame ...)`
   results are ignored. **Fix**: if either fails, `Log` with `GetLastError()` and `return 1` from
   `Serve` (the game's `HostLost` respawns).

Optional, same file: the CPU `h.fence_out->Signal(fm.n)` fallbacks (~:1895, ~:1967) can move the
fence backwards while a GPU `h.queue->Signal(h.fence_out, n-1)` is still pending. If touched: only
CPU-signal after `WaitFenceValue(h.fence, h.fence_value, 100)` succeeded, otherwise
`h.queue->Signal(h.fence_out, fm.n)`. Low priority; self-heals on the next frame.

### 1.12 Shader: the camera-fit passes run every frame with geometry off (LOW, free win)

`shaders/DLSS5_Feed.fx`: passes `FitSamples` and `FitSolve` (~:803-804) run unconditionally;
`PS_FitSolve` (~:538) is a single-pixel shader that loops `2 x DLSS5_FIT_W*DLSS5_FIT_H` (= 1840)
iterations with two fetches each plus a 9x9 solve, and its result is only consumed under
`if (GEOM_ENABLE && FitIsUsable())` (~:685). `GEOM_ENABLE` defaults to false.

**Fix**: first line of `PS_FitSamples` and `PS_FitSolve`: `if (!GEOM_ENABLE) { <all outs> = 0.0; return; }`
(uniform branch, no divergence; ReShade cannot skip a pass per uniform). Make sure `FitIsUsable`
still returns false on the zeroed `Cam5` (it requires `s.z >= 40`, so yes).

---

## Phase 2: stuck keys and buttons in the cast panel (32-bit)

`CastInput` (`src/dlss5-feed32.cpp` ~:1280-1387) posts `WM_KEYDOWN`/`WM_KEYUP` and the mouse
button messages to the host window, but only while the cursor is inside the panel or a button is
captured (`if (!inside && !g_cast_captured) { ...; return; }` ~:1328). A key held when the cursor
leaves the panel, or when the panel is hidden (toggle key, Escape, close button, Alt+F4,
`CastHostLost`), never gets its `WM_KEYUP`, so ReShade x64's ImGui keeps it down: later clicks
become Ctrl+clicks, text fields keep repeating, Shift sticks.

**Change**:
1. Add `static bool g_cast_key_down[256];` and `static UINT g_cast_btn_down;` (bit mask of the
   `kButtons` indices) next to the other `g_cast_*` globals (~:933-962).
2. In the forwarding loop (~:1371-1386): on `WM_KEYDOWN` set `g_cast_key_down[vk] = true`; on
   `WM_KEYUP` clear it. Same for the button loop (~:1354-1358).
3. New `static void CastFlushInput()`: for every `vk` with `g_cast_key_down[vk]`, `CastPostKey(WM_KEYUP, vk, true)`
   and clear; for every set button bit post its `up` message at `g_cast_last` (or `MAKELPARAM(0,0)`)
   and clear. Guard `g_cast_hwnd != nullptr` (also add that guard inside `CastPostKey`, ~:1155).
4. Call `CastFlushInput()`:
   - at the hover-exit return (~:1328) and the close-button return (~:1314), only if anything is down;
   - at the start of `CastRelease()` (~:977) — covers toggle key, Escape, close, Alt+F4, host lost,
     DLL detach, and the `!g_cast_wanted` branch of `CastTick`.
5. Log once per flush at `log_frames` level, e.g. `[feed32] cast: released N keys / M buttons`.

Verification (user, Fable): hold Ctrl, click a slider in the panel, move the cursor out of the
panel, release Ctrl, move back in and click another slider: it must NOT open the text-entry box.
Hold a letter in a text field, press the toggle key: the field must stop repeating when the panel
is shown again.

### 2b. Mouse wheel does not scroll inside the cast panel (user report, 2026-09-02)

Symptom: with the host's panel cast into the 32-bit game window, clicks, drags and keys reach
ReShade x64, but the wheel never scrolls its windows (the Add-ons tab, the variable list), so a
long DLSS 5 panel cannot be scrolled from the game.

Code: `CastInput` reads the wheel with `rt->get_mouse_cursor_position(&cx, &cy, &wheel)`
(`external/reshade/include/reshade_api.hpp:151`: "the mouse wheel delta since the last frame", in
notches) and posts
`PostMessageW(g_cast_hwnd, WM_MOUSEWHEEL, MAKEWPARAM(mk, wheel * WHEEL_DELTA), MAKELPARAM(sp.x, sp.y))`
with `sp` = `GetCursorPos()`, i.e. the REAL cursor in screen coordinates (over the game window),
while every other message is posted with the panel-mapped host-client position `at = MAKELPARAM(hx, hy)`.

Diagnose first (five minutes in Fable): add a `Log("[feed32] cast: wheel %d", wheel)` when
`wheel != 0`. That splits the problem in two:

- **Wheel is non-zero (likely).** The host side drops or misplaces it. A real `WM_MOUSEWHEEL`
  carries the cursor in *screen* coordinates of the target window, and ReShade x64 derives its
  ImGui mouse position from the message it is handed; the real cursor (over the game) maps to
  the wrong host-client position, so the scroll lands on nothing and the hover is lost until the
  user moves the mouse (`WM_MOUSEMOVE` is only re-posted when `hx/hy` change). Fix: post
  `WM_MOUSEMOVE(at)` immediately before the wheel, then the wheel with
  `POINT s = { hx, hy }; ClientToScreen(g_cast_hwnd, &s);` in `lParam`, then reset
  `g_cast_last = { -1, -1 }` so the next frame re-posts the move. Also post one message per notch
  (`for |wheel| times, delta = ±WHEEL_DELTA`) rather than a multiple, which some ImGui versions clamp.
- **Wheel is always zero.** ReShade x86 resets the delta before `reshade_present` or does not
  accumulate it while `block_input_next_frame()` is active. Two fallbacks, in order: (1) read the
  wheel in the `reshade_overlay` callback (`OnOverlay`, which runs inside ReShade's `draw_gui`,
  before its `next_frame` reset) and cache it in a global that `CastTick` consumes; (2) if that is
  zero too, install a `WH_GETMESSAGE` hook on the game window's thread
  (`SetWindowsHookExW(WH_GETMESSAGE, proc, g_self, GetWindowThreadProcessId(g_cast_dest, nullptr))`)
  while the panel is shown, accumulate `WM_MOUSEWHEEL` deltas (and `RI_MOUSE_WHEEL` from `WM_INPUT`
  via `GetRawInputData` for raw-input games) into an `InterlockedAdd` counter that `CastInput`
  drains; unhook on hide, `CastRelease` and detach. The hook sees the messages before ReShade
  nulls them for the game.

Verification (user, Fable): hover the DLSS 5 add-on's page in the cast panel and scroll: the page
scrolls in both directions and stays under the cursor; scrolling over a slider does nothing else;
after hiding the panel the game's own wheel behaviour is back.

---

## Phase 3: the 32-bit render thread never blocks on the host

### Problem (all in `src/dlss5-feed32.cpp`, line numbers at HEAD)

`EnsureHost()` (~:1588-1665) is called from the three build functions (`BuildShared` :2044 at
:2105, `BuildSharedGl` :2284 at :2301, `BuildSharedVk` :2450 at :2474), which run on the render
thread inside `reshade_render_technique` (`FeedFrame` :3542 -> `FeedFrameDispatch`, under the
non-recursive `g_feed_cs`). It `CreateProcess`es the host, spins `CreateFileA` + `Sleep(100)` for
up to 15 s (:1625-1631), blocks on the hello ack (:1648), and each build then blocks on the 'B'
ack (:2127-2130 / :2317-2320 / :2490-2493), which the host answers only after NGX init and the
feature create (seconds; the ~165 MB model load on a cold host). Every Apply, restart, resize and
work-resolution change freezes the game for that long, and the overlay text (~:1659-1665) claims
the opposite. `PipeWrite`/`PipeRead` (:1498-1521) are plain `WriteFile`/`ReadFile` with no timeout,
so a hung-but-alive host blocks the game forever: the build ack never comes, or the 21-byte
per-frame write (`PipeWriteFrame`, callers :3001 GL, :3269 Vk, :3498 D3D11) blocks once the host's
1 KB pipe buffer is full. `HostAlive()` cannot see a hang.

### Design

**New state** (next to the `Feed32 g` struct):
```cpp
struct HostLink {
    HANDLE thread;            // worker, nullptr when idle
    volatile LONG state;      // LINK_IDLE, LINK_RUNNING, LINK_DONE, LINK_FAILED (Interlocked*)
    volatile LONG abort;      // set by the render thread; the worker checks it between steps
    HANDLE abort_event;       // wakes the worker's waits
    int job;                  // JOB_CONNECT (spawn+pipe+hello), JOB_BUILD (send 'B', read ack)
    FeedBuild build;          // input of JOB_BUILD, filled by the render thread
    FeedBuildAck ack;         // output, valid when state == LINK_DONE
    char why[160];            // failure text for HostLost / FeedDisable
    DWORD ms;                 // how long the job took (for the log)
};
static HostLink g_link;
```
Only the worker writes `ack`, `why`, `ms`; only the render thread reads them, and only after it
observed `LINK_DONE`/`LINK_FAILED` through `InterlockedCompareExchange`. `g.pipe`, `g.hproc`,
`g.panel_w/h` are set by the worker for JOB_CONNECT (the render thread does not touch them while
`state == LINK_RUNNING`).

**Pipe with timeouts.** Open the pipe with `FILE_FLAG_OVERLAPPED`. Replace `PipeWrite`/`PipeRead`
by `PipeXfer(bool write, void *buf, DWORD len, DWORD timeout_ms)`: `OVERLAPPED` + a per-call
event (or one cached event per direction), `WriteFile`/`ReadFile`, then
`WaitForMultipleObjects({ev, g_link.abort_event}, timeout)`; on timeout or abort `CancelIoEx` and
return false with the reason. Loop until `len` bytes moved (byte-mode pipe). Timeouts:
hello 15 s (host cold start, ReShade + NGX), build ack 60 s (model load), per-frame write 250 ms.
A timed-out frame write means the host stopped reading: `HostLost("the host stopped responding")`
(which TerminateProcesses it and lets the retry/respawn path run).

**Worker** (`static DWORD WINAPI HostWorker(void *)`), one job per thread lifetime, `CreateThread`
in `HostSubmit(job)`; the render thread joins with `WaitForSingleObject(thread, 0)` on each poll
and closes the handle when done.
- JOB_CONNECT = today's `EnsureHost` minus the fast path: exe path, `CreateProcessA`, the
  `CreateFileA` loop with `Sleep(100)` (returns early on `abort`), the self-handle duplication,
  hello write + ack read (through `PipeXfer`), version check, `g.panel_w/h`. On any failure fill
  `why` and set `LINK_FAILED`; `RestoreGameFocus()` moves to the render thread's completion handler.
- JOB_BUILD = `PipeXfer('B')`, `PipeXfer(build)`, `PipeXfer(read ack)`.

**Render-thread split of each build function** (D3D11 `BuildShared`, `BuildSharedGl`, `BuildSharedVk`):
- *Prepare + submit* (unchanged code up to the 'B' exchange): `ReleaseShared()`, size globals,
  local textures (D3D11), `CastMakePanel()`, fill `FeedBuild b`. Then: if no live link
  (`g.pipe == nullptr || !HostAlive()`) submit JOB_CONNECT with `b` stored for a follow-up
  JOB_BUILD; else submit JOB_BUILD. Set `g.build_pending = true`, log
  "build: handed to the host (game keeps rendering)", and return `false` WITHOUT `FeedFail`.
- *Poll* at the top of `FeedFrameDispatch` (under `g_feed_cs`), before the `!g.built` check:
  `HostPoll()`: if `state == LINK_DONE`: JOB_CONNECT -> log "host connected in N ms",
  `RestoreGameFocus()`, submit the stored JOB_BUILD; JOB_BUILD -> call the API's *finish* function
  with `g_link.ack`. If `LINK_FAILED`: `HostLost(why)` for connect/exchange failures (as today),
  or the version-mismatch `FeedDisable` text. Then `g.build_pending = false`.
- *Finish* (`BuildFinish11/Gl/Vk(const FeedBuildAck &ack)`): the code that today follows the
  `PipeRead(&ack)`: GL/Vk handle capture before any early return, the `SR_UNAVAILABLE` case
  (set `g.sr_unavailable = true` and re-run *prepare + submit* instead of recursing),
  `!ack.ok` -> `FeedFail("host build")` (backoff via `g_retry_at` as today), D3D11
  `host_creates` opens, fence import (`OpenSharedFence` / GL / Vk semaphores),
  `CastImportPanelGl/Vk`, `g.built = true; g.need_reset = true; g.out_valid = false;`.
- While `g.build_pending` the frame path returns early (no feed, no `FeedFail`), and the overlay
  status shows "waiting for the host (N s)".

**Overlay and teardown.** `HostRestart` / `HostApplySettings` / Re-enable (DrawOverlay) no longer
call `HostClose` directly: they set `g_host_request` (RESTART / APPLY / REENABLE) and the
existing focus capture; `FeedFrameDispatch` consumes the request under `g_feed_cs` before the
poll. `HostClose` first stops the worker: `InterlockedExchange(&g_link.abort, 1)`,
`SetEvent(abort_event)`, `CancelIoEx(g.pipe, nullptr)`, `WaitForSingleObject(thread, 3000)`; then
the existing sequence. `DLL_PROCESS_DETACH` does the same through `HostClose`. The 4 s
`WaitForSingleObject(g.hproc, 4000)` in `HostClose` stays (user-initiated restarts only).

**Preserved behaviour**: `FeedFail` backoff, `HostLost` semantics (`HostClose` + `FeedDisable`),
the GL/Vk "own every ack handle before any early return" rule, the D3D11 "keep `built` across host
restarts" rule (plus Phase 1.8), `g.panel_w/h` from the hello ack, the SR fallback build.

**Risks to watch**: the worker must never touch ReShade or the game's device (it only does Win32 +
pipe); `CreateProcess` from a worker while the game is inside Present is fine (no loader lock);
after a `CancelIoEx` the pipe is still usable for the next transfer, but a timed-out *build* leaves
the host mid-create, so treat it as `HostLost` (kill and respawn), never as a retry on the same
pipe; `FeedEnter`'s busy flag means the poll must not re-enter the frame path.

### Verification (user)

- Fable (D3D11): game start -> the game renders normally while the host starts; log shows
  "handed to the host", then "host connected in N ms", "build finished in N ms". Apply / Restart
  from the overlay: no freeze. Kill the host in Task Manager: the game keeps running, `HostLost`
  logged, Re-enable brings it back. Suspend the host in Process Explorer: within ~1 s the log
  says the host stopped responding, the game keeps running, resume + Restart works.
- Worms (GL) and Castlevania under DXVK (Vulkan): start and restart as above; texture cast still
  works after a restart (panel re-imported).
- Host `--test --hide` unchanged (it does not use the add-on).

---

## Verification (whole plan)

Builds (`build.bat`, `build-addon32.bat`, `host\build-host.bat`): zero warnings/errors. Host
`--test --hide`: 300/300.

Game checks the user runs (deploy per DEPLOY-DEV.md, keep the replaced binaries in a `prev-*`
subfolder as before):

| Item | Game | What to see |
|------|------|-------------|
| 1.1 | Metro 2033 Redux (64-bit D3D11) or any same-device D3D12 game | Change the resolution in-game: the log shows one "holding the feature (re)build" line, then "building:", and DLSS resumes. Before: the line repeats every frame. |
| 1.2 | Metro 2033 Redux | Change Preset / HDR / Depth inverted in the overlay: a "building:" line follows within 60 frames. "Force one rebuild" does the same. Switch Mode 1 -> 2: no "evaluate failed" line. |
| 1.5 | any | `dlss5-feed.log` still writes a crash dump on a forced crash (unchanged); no regression visible otherwise. |
| 1.8 | Fable | Kill `dlss5-feed-host64.exe` in Task Manager, then press Re-enable in the overlay: the host respawns and the feed resumes. |
| 1.9 | Castlevania LoS (DXVK) | Alt-tab / change resolution so DXVK recreates the device with texture cast on: no crash, panel returns. |
| 1.11 | Fable | Work-resolution changes with the panel shown: no device-removed; `dlss5-feed-host.log` clean. |
| 1.12 | any | ReShade.log has no compile error for DLSS5_Feed.fx; with GEOM_ENABLE on, the debug view still shows the fit strip. |
| Phase 2 | Fable | The two stuck-key scenarios above. |
| 0a/0b | The Surge 2 | After the A/B confirmed the static test: with the hysteresis shader the judder is gone with `VALIDATE_STATIC=1`; the debug view shows no per-frame flip of zeroed vectors on a slow pan. |
| 0c | The Surge 2 | No "external frame pacer" line unless Smooth Motion is really on; the present probe reports ~1.00x. |
| 0d | any | Force a crash (or wait for the depth-copy one): exactly one "CRASH RECORDED" line and a non-empty `dlss5-feed-crash.dmp`. |
| Phase 3 | Fable, Worms, Castlevania | The list in Phase 3. |

## Not doing (reviewed, rejected or downgraded)

- FSR 1 RCAS on HDR backbuffers: the gate is missing but no visible effect was confirmed. Try
  before changing.
- Host `hello.pid` vs argv pid check: local same-user pipe; hygiene only. Add if convenient (one
  `if (hello.pid != game_pid) return 1;` in `Serve`).
- `RenderTargetWriteMask = 4` comment vs Launchpad's channel: unverifiable without Launchpad's source.
- The four duplicated `FeedFrame*` bodies in the 64-bit add-on: real maintenance cost (1.1 and 1.2
  are four-site fixes because of it) but a refactor is not worth the regression risk right now.
