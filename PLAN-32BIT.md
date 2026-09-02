# DLSS5-Feeder for 32-bit games — out-of-process host plan

## Context

The feeder currently requires a 64-bit game: NGX (`nvngx_dlss.dll`, the driver's `_nvngx.dll`, and the
DLSS 5 model `nvngx_dlssnr.dll`) exists only as x64, and ReShade inside a 32-bit game is itself 32-bit —
it loads `.addon32` only, so neither our `dlss5-feed.addon64` nor `renodx-dlss5.addon64` can even load.
That rules out BioShock Remastered, Splinter Cell Blacklist (both 32-bit D3D11) and most pre-2015 titles.

The way through is the one thing about the current design that does not care about bitness: **WDDM shared
resources**. Our D3D11 path already moves every frame between two devices over shared NT-handle textures
and a shared fence (`MakeSharedPair`, `OpenSharedFence` in `src/dlss5-feed.cpp`). Those same kernel
objects can cross a **process boundary** exactly as they cross a device boundary — a 32-bit game process
and a 64-bit helper process on the same adapter can open each other's shared textures and fences. No
frame data ever touches system memory or the pipe; the GPU copies stay GPU copies.

So: split the current add-on in half at the exact seam where the shared handles already sit.

```
 32-bit game process                                64-bit helper process (dlss5-feed-host64.exe)
 ┌──────────────────────────────┐                   ┌──────────────────────────────────────────┐
 │ ReShade x86 (dxgi.dll)       │                   │ ReShade x64 (dxgi.dll) + renodx-dlss5    │
 │  LaunchPad + DLSS5_Feed.fx   │   control pipe    │  hidden 1x1 window + tiny D3D12 swapchain │
 │  dlss5-feed.addon32:         │ ◄───────────────► │  private D3D12 device + NGX (DLAA)        │
 │   copy color/depth/MV into   │  handles, sizes,  │  waits shared fence, evaluates,           │
 │   shared textures, Signal ───┼── shared fence ───┼─► NR addon hooks inline, Signal back      │
 │   Wait, blit Output → bb     │  (GPU-side sync)  │                                           │
 └──────────────────────────────┘                   └──────────────────────────────────────────┘
```

## 1. Architecture

Two artefacts replace the single add-on for this scenario:

* **`dlss5-feed.addon32`** — the in-game half. It is the current add-on *minus* everything D3D12/NGX:
  ReShade event handling, `ResolveHandles`, cfg/log, the three `CopyResource` calls into the shared
  textures, `ctx4->Signal`/`Wait` on the shared fence, and `BlitOutputToBackbuffer`. All of that code is
  already 32-bit-clean C++ on D3D11 interfaces. Add a small pipe client and a process babysitter.
  Built from the same `dlss5-feed.cpp` with an `FEED_X86` guard rather than a fork — the shared code
  (Cfg, Log, blit, handle resolution) stays one source of truth.
* **`dlss5-feed-host64.exe`** — the out-of-game half. Owns the D3D12 device, the NGX session, the
  feature, the evaluate loop. This is `InitSession` + `BuildResources` + `CreateDlssFeature` + the
  evaluate half of `FeedFrame`, moved almost verbatim, plus a pipe server.

A free structural win: **crash isolation gets better than in-proc.** The leaked NR add-on faulting now
kills only the helper; the add-on sees the broken pipe, restores the frame path, and the game never
notices. The SEH guards remain, but the blast radius shrinks to a process we own.

## 2. Transport

**Who creates what.** The host creates all four textures (Color, Output, Depth R32F, MV RG16F) as D3D12
committed resources with `D3D12_HEAP_FLAG_SHARED` + `CreateSharedHandle` — the same first branch
`MakeSharedPair` already takes. The host also creates the two fences (`in_fence`, `out_fence`) with
`D3D12_FENCE_FLAG_SHARED`. The game side opens textures via `ID3D11Device1::OpenSharedResource1` and
fences via `ID3D11Device5::OpenSharedFence`, exactly as today.

**Getting handles across.** `CreateSharedHandle` produces NT handles; two standard routes:
1. **DuplicateHandle** (preferred): the add-on sends its PID in the hello message; the host calls
   `OpenProcess(PROCESS_DUP_HANDLE)` (same user, always granted) and `DuplicateHandle` for each of the
   six objects, then sends the duplicated handle *values* over the pipe. On Win64 kernel handles fit in
   32 bits by contract, so the values are valid in the 32-bit process as-is.
2. Named objects (`CreateSharedHandle` with `lpName` + `OpenSharedResourceByName`) as fallback — works
   for textures, but D3D11 has no open-fence-by-name, so route 1 is needed for fences anyway.

**Verify-first item (phase 0 spike):** a 32-bit D3D11 process opening a shared handle created by a
64-bit D3D12 process. WDDM shared allocations are documented bitness-agnostic and this is how the
compositor works, but it has never been exercised by this codebase — prove it with two 50-line test
programs before building anything on top. Same spike proves the shared fence. If `ID3D11Device5`
(Win10 1703+) fails to QI on some ancient game device, fall back to keyed-mutex textures
(`D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX`) — slower, but universal.

**Control protocol** (named pipe `\\.\pipe\dlss5-feed.<game pid>`, fixed-size binary structs):
* `Hello { pid, version }` → `HelloAck { version }`
* `Build { width, height, color_fmt, output_fmt, hdr, depth_inverted, flags, mv_scale }` →
  `BuildAck { 6 duplicated handles, ok, ngx_result }` — sent on every resolution/format change;
  host tears down and recreates, mirroring `BuildResources`.
* `Frame { n }` → nothing (completion is the fence, not a pipe message).
* Pipe break in either direction = shutdown signal for that side.

The allocator/fence ring stays host-side, unchanged (`BeginCommands`/`EndCommands`, ring of 3).

**Protocol evolution since.** The shipped header is `src/feed_ipc.h`; the version is enforced
exactly on both sides, because the message structs change size between versions and a mismatched
pair would not merely misbehave, it would desync the pipe.

| Version | Shipped in | Change |
| --- | --- | --- |
| 1 | 0.5.x | As designed above: D3D11 only. The game creates the four shared textures and sends its own handle values; the host duplicates them out and opens them on D3D12. |
| 2 | 0.7.0 | `FeedHello::client_kind` and `FeedBuildAck::tex[]` / `tex_size[]`, for the **OpenGL** client — where the direction has to flip, because GL memory objects are import-only. The host creates and duplicates the handles *in*. A v2 host still reads a v1 client's shorter hello (`FEED_HELLO_V1_SIZE`) so it can refuse it with a real message rather than blocking on bytes that never come. |
| 3 | (this release) | `FEED_CLIENT_VULKAN`, which reuses v2's host-creates direction unchanged — D3D12 cannot open Vulkan-exported memory either — plus `FeedBuildAck::output_fmt`. That last field is the only genuinely new idea: when the host owns the textures it also owns the Output *format*, because the typed-UAV-store fallback for BGRA8 can only be decided by a D3D12 device, and the game needs the real answer to import the image and to choose copy-vs-blit for the way home. |

The direction question is the driver's, not ours, and the answer is per-API: D3D11 exports and
D3D12 opens; GL and Vulkan cannot be opened by D3D12, so the host creates instead. Host-created is
the better resource anyway — it is born with exactly the D3D12 flags NGX wants.

## 3. The NR add-on inside the helper

`renodx-dlss5.addon64` is a ReShade add-on; a bare helper process has no ReShade. Options:

* (a) Host a minimal ReShade runtime — heavy, tracking a moving target. No.
* (b) LoadLibrary the add-on and synthesize the add-on API it registers against — fragile, we would be
  reimplementing `register_event`/`init_device` semantics for a closed binary. No.
* (c) **Make the helper look like a game** — put ReShade x64 (`dxgi.dll`) and `renodx-dlss5.addon64`
  next to `dlss5-feed-host64.exe`. The helper creates a hidden 1×1 window and a minimal D3D12 swapchain:
  ReShade loads, installs its hooks, enumerates the NR add-on. Then the helper creates its NGX device
  through `d3d12.dll` exports — the very mechanism already proven in Metro 2033 Redux, where our
  in-proc private device was created the same way and the NR add-on saw `init_device` and hooked the
  inline evaluate. Pump an occasional `Present()` on the dummy swapchain so ReShade's per-frame
  bookkeeping runs.

**Recommendation: (c).** It reuses the exact code path that already works, treats the NR add-on as the
black box it is, and keeps the helper ~200 lines of scaffolding instead of a runtime reimplementation.
Directory layout: `host64\` subfolder next to the game exe carrying the helper exe, ReShade x64,
`renodx-dlss5.addon64`, `nvngx_dlssnr.dll`, `nvngx_dlss.dll`.

## 4. Sync model

Two workable models, both GPU-side only (no CPU wait in the game's frame):

* **A — same-frame (recommended first):** game copies inputs, `Signal(in_fence, n)`, immediately records
  `Wait(out_fence, n)` + blit on its immediate context. Identical to the in-proc design today — the
  game's GPU queue stalls only for the evaluate itself (~1–3 ms). Cross-process adds scheduler jitter,
  but the wait is on the GPU timeline, not a thread.
* **B — pipelined (+1 frame):** game blits the *previous* frame's output (`Wait(out_fence, n-1)`), so the
  two processes never serialize within a frame. Costs one frame of NR latency (the DLAA history makes
  this mostly invisible) and hides all jitter. Keep as a cfg knob (`pipeline=1`) and flip to default if A
  shows hitching in practice.

> **Status (2026-09-01):** A shipped first and stalled exactly as feared — a hard ~35 fps ceiling at any
> resolution (issue #15). **B is now the default**, as `async_home=1` (named for the 64-bit add-on's
> matching knob), with A back under `async_home=0`. Two details differ from the sketch above: the wait
> targets the last frame sent to the *current host session*, not `n-1` — the fences restart at zero with
> every host respawn, and a stale value would deadlock into a TDR — and the blit is recorded *before*
> `Signal(in_fence, n)`, which is what lets one shared Output slot stay race-free. The host loop below
> also lost its CPU wait: `queue->Wait(in_fence, n)` alone orders the evaluate, the serve loop wakes on
> an overlapped pipe read instead of a `Sleep(8)` poll, and the banner Present left the frame path.

## 5. Estimate, risks, phases

**Estimate.** Host exe ~600 lines (mostly relocated, not new), `.addon32` delta ~400 lines (pipe client,
babysitter; the rest is existing code behind `FEED_X86`), protocol header ~100, `build.bat` gains a
vcvars32 target (no NGX lib on the x86 link — it is x64-only, which is the whole point). Two spike
programs. Realistically 2–4 working sessions to a Blacklist proof.

**Risks, honest.**
* The phase-0 spike failing (32↔64 shared open) sinks the plan as designed — fallback is keyed mutex +
  `D3D9Ex`-era sharing, considerably worse. Believed very unlikely.
* The NR add-on's assumptions in a near-empty host process (it is closed-source; it may key off swapchain
  properties or game heuristics). Mitigation: the dummy swapchain makes the host *be* a plain D3D12 app,
  and the warmup-rebuild trick already in the feeder covers its first-create latch.
* Old-game D3D11 runtimes: `ID3D11Device1`/`Device5` QI must succeed on the *game's* device (OS-provided
  on Win10/11 regardless of feature level — expected fine, verified in the spike).
* D3D9 32-bit games are **out of scope** for this plan: no shared fences, restricted shared formats.
  Possible later via D3D9Ex shared surfaces + flush-based sync; separate plan if ever.
* WOW64 itself: no relevant quirks — handles are 32-bit-safe by contract, and the pipe/duplication
  pattern is standard.

**Phases.**
1. **Spike:** two tiny programs — x64 creates shared texture + fence, x86 D3D11 opens both, GPU copy
   round-trips a pixel, fence signals observed. Go/no-go.
2. **Host:** `dlss5-feed-host64.exe` + ReShade + NR add-on standing alone: dummy swapchain up, NGX
   session up, `feature 18 created` in the host's ReShade.log with a synthetic test pattern.
3. **Transport:** `.addon32` in Splinter Cell Blacklist, `mode=1` — frame round-trips through the host
   untouched (proves handles, fences, blit, lifetime, pipe).
4. **Full path:** DLAA + NR in Blacklist; `MV_SIGN`/debug view validation as done for Metro.
5. **Polish:** auto-spawn + watchdog both ways, cfg forwarding, README, GitHub release with both
   binaries.
