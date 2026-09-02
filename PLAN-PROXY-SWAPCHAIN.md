# PLAN: a proxy swapchain — the only route to a real frame-rate win (issue #34)

Status: **design + spike only. No add-on code.** `spike\spike-proxy-swapchain.exe` proves the
COM contract in an empty process; nothing here is scheduled.

## Why this exists

Issue #34 asks for DLSS Quality/Balanced/Performance. Everything the feeder can do after the
game has rendered (`work_resolution`, `work_upscale=1` FSR 1, `work_upscale=2` synthetic-jitter
DLSS SR) is bounded by two facts: the game already paid for every native pixel, and the native
frame is the information ceiling. The one way to make a DLSS-less game *render fewer pixels* is
to make it believe its backbuffer is smaller than what reaches the display. That is what
Lossless Scaling and Magpie do from outside the process (a second window on top). Doing it
in-process is a proxy swapchain.

## The model

```
game --CreateSwapChain(1440p)--> proxy --CreateSwapChain(4K, window size)--> DXGI
game --GetBuffer(0)-----------> proxy returns a private 1440p texture
game --renders at 1440p-------> private texture
game --Present()--------------> proxy: feeder pipeline (DLSS SR / FSR 1 / EASU) 1440p -> real 4K backbuffer,
                                       then real Present()
```

The game keeps the resolution it chose; the window / display stays at the desktop size. In the
spike the game renders 960x540 and the display is 1920x1080 (F toggles EASU vs bilinear, the
difference is obvious on the thin lines).

What the spike proves (`spike-proxy-swapchain.exe --frames 90` → `PASS`):

- A `IDXGISwapChain` wrapper that lies in exactly three places — `GetBuffer`, `GetDesc`,
  `ResizeBuffers` — and forwards the rest keeps a normal D3D11 render loop working.
- `Present` can run a full upscale pass on the real backbuffer before the real `Present`.
- A window resize re-fits the real chain while the game's private buffer keeps its size.
- The frames reach the display (readback of the real backbuffer sees the pattern).

What it does not prove: anything about a game or the hook stack. That is the whole problem.

## What a shipping version needs — and why it is not planned

### 1. A seat at the factory

ReShade's `create_swapchain` event (`external/reshade/include/reshade_events.hpp:1843`) can
only *edit the desc*; whatever size it settles on is what the game renders into. Substituting
the buffer object needs `CreateSwapChain*` on the factory (vtable or export hook) returning the
wrapper. Three parties already sit there:

- **ReShade** hooks the factory to wrap the swapchain for its own runtime.
- **NvPresent64 (Smooth Motion)** hooks `CreateDXGIFactory*` and wraps `IDXGISwapChain` in its
  own COM proxy, and adds a second D3D11 device + invisible swapchain
  (`PLAN-SMOOTHMOTION.md:58-66`, issue #1's two-runtime bug, `src/dlss5-feed.cpp:631-648`).
- **The game's own overlay stack** (Steam, Discord, RTSS, GeForce overlay) hooks `Present`.

Hook *order* decides who wraps whom. If the feeder wraps inside ReShade, ReShade's runtime
sees the 4K real chain and its effects (including `DLSS5_Feed.fx`, which produces the motion
vectors) run at 4K on an already-upscaled frame — useless. If the feeder wraps outside ReShade,
ReShade sees the 1440p private chain, which is what we want, but then ReShade must be loaded
*after* us, i.e. we can no longer be a ReShade add-on loaded by ReShade. NvPresent adds a
third layer whose order depends on driver settings.

### 2. Moving the feeder off `reshade_render_technique`

Today the entire feed runs inside ReShade's technique callback with the RTV ReShade hands it
(`OnRenderTechnique`, `FeedFrame`). With a proxy the upscale has to happen at the wrapper's
`Present`, after ReShade's effects rendered on the private buffer. The technique would only
produce guides (motion vectors, depth, mask) and the Present hook would consume them; the
D3D11-64 path would need the DLSS output written to the real backbuffer instead of the
ReShade RTV, and the D3D12 / Vulkan / OpenGL paths are out entirely (Vulkan presents live inside
the ICD, `PLAN-SMOOTHMOTION.md:25-29`; a D3D12 proxy is a separate wrapper with its own
command-queue ownership questions).

### 3. Interface surface

The spike answers `IDXGISwapChain` only. Games use `IDXGISwapChain1..4`: `Present1`,
`GetDesc1`, `SetColorSpace1`, `GetCurrentBackBufferIndex`, `SetMaximumFrameLatency`,
`GetFrameLatencyWaitableObject`, HDR metadata. Every one is a forwarding method, but every one
is also a place to get a subtle lie wrong (e.g. `GetCurrentBackBufferIndex` on a flip-model
chain the game thinks has 3 buffers while ours has 2).

### 4. Fullscreen and mode switches

In exclusive fullscreen the game calls `SetFullscreenState`/`ResizeTarget` to set a 1440p
mode. The proxy must refuse the mode switch (keep the desktop mode) while telling the game it
succeeded; some engines then read back the output's mode and disagree with themselves. Borderless
is the sane target and should be the only supported one.

### 5. State ownership

The wrapper renders on the game's immediate context at Present time. The spike gets away with
`ClearState()`; a shipping wrapper saves and restores exactly what it touches (the feeder's
`BlitOutputToBackbuffer` already has that discipline) and must be reentrant against overlays
that also draw at Present.

## Recommendation

Do not build this into the add-on. The payoff (the game renders fewer pixels) is real, but the
cost is a new hook layer in the one place three other hook layers already fight, and a
restructuring of every render path. The same payoff is available today at zero risk:

> Set the game to the lower resolution and let the GPU scale it to the panel (NVIDIA Control
> Panel → Adjust desktop size and position → GPU scaling). The feeder then runs 1:1 at that
> resolution, exactly as it does now.

Revisit only if a concrete game shows the GPU-scaling route failing (some engines refuse
non-native resolutions in borderless) *and* Smooth Motion is off for that game (so NvPresent is
out of the hook stack). At that point the spike's wrapper is the starting point, and the first
task is the hook-order experiment, not more wrapper code.

## Files

- `spike/spike-proxy-swapchain.cpp` — the wrapper + a fixed-resolution "game"; built by
  `spike\build-spike.bat`; `--frames N` for scripted runs, `--bilinear` for the comparison.
- `src/feed_fsr1.h` — the EASU pass the spike presents through (shared with `work_upscale=1`).
