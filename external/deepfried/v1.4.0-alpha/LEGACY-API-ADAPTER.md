# Legacy API adapter boundary

This is the implementation boundary for x64 D3D9Ex and D3D10. It is not a
claim that those backends are working in 1.3.4 alpha.

```text
D3D9Ex or D3D10 game
  -> legacy shared Color/Depth/Motion/Output textures
  -> private D3D11 relay and format conversion
  -> NT shared textures plus D3D11/D3D12 shared fence
  -> private D3D12 device, NGX, and Deep Fried Chicken
  -> reverse both boundaries before ReShade effects/present
```

## Required lifecycle

- ReShade `create_device` (event 96): when `api == device_api::d3d9` and the
  mutable `api_version == 0x9000`, set `api_version = 0x9100` and return `true`
  before device creation. Do not try to replace the by-value `api` enum. If
  attachment is late, request a restart.
- `init_device` / `destroy_device` (0 / 1): own relay devices and queues.
- `init_swapchain` / `destroy_swapchain` (6 / 8): build and retire the ring.
- `init_effect_runtime` / `destroy_effect_runtime` (9 / 10): discover and
  validate depth/motion semantics.
- `reshade_begin_effects` (76): preferred final-frame injection point.
- `present` (74): lower-level proxy fallback only.
- `finish_present` (100): diagnostics only; it is too late to replace the
  frame that was just presented.

## Transport rules

- Match the host adapter LUID for every private device.
- Keep Color, Output, Depth, and Motion Vectors as distinct resources.
- D3D9Ex permits only a narrow legacy-sharing format set. Pack depth and motion
  into a shareable format such as RGBA16F, then convert on D3D11 to R32_FLOAT
  and R16G16_FLOAT.
- D3D10.1 can use `IDXGIKeyedMutex`; D3D10.0 needs explicit Flush/event-query
  synchronization. Neither can directly join the D3D11/D3D12 NT shared fence.
- The modern half uses a D3D12 shared fence opened through `ID3D11Device5` and
  signalled through `ID3D11DeviceContext4`.
- Use at least three command allocators and retire them by fence value. Never
  free or resize a shared resource until both API sides are idle.
- Fail closed on plain D3D9, MSAA, unsupported formats, missing/flat depth,
  missing motion vectors, adapter mismatch, or a failed fence transition.

## Honest quality boundary

D3D9 and D3D10 games do not publish an NGX DLSS parameter block. A final
backbuffer alone can support only a synthetic full-resolution DLAA experiment.
Performance/Balanced/Quality require an earlier lower-resolution Color target
plus matching depth, motion, jitter, exposure, and output dimensions. The UI
must not expose those quality modes until that real render contract has been
captured and validated.

The pinned official v1.1.0 bridge source under
`Tools\DlssNrCascade\reference\dlss5-bridge-v1.1.0` is the reference for the
D3D11-to-D3D12 shared-resource/fence half. It explicitly supports D3D11,
D3D12, and Vulkan, not D3D9 or D3D10.
