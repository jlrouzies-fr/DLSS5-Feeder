# Deep Fried Chicken 1.4.0 alpha

This is an experimental ReShade add-on for applying one to thirty sequential
NVIDIA DLSS Neural Rendering passes after a game's DLSS Super Resolution
output. Native DLSS remains the only spatial upscaler and safe fallback.
Chicken feeds its completed output-resolution frame, along with the game's
depth and motion-vector guides, into the user-supplied NVIDIA DLSSNR runtime.

Chicken is not a custom-trained model, a texture pack, or a replacement for
the game's renderer. Extra passes can reveal or reconstruct convincing detail,
but they can also oversharpen, shimmer, smear, invent detail, or reduce frame
rate dramatically. Results depend on the quality of each game's DLSS contract.

## Main changes in 1.4.0

- One to thirty live-selectable passes. Every pass has the same NR preset, style,
  intensity, local tone, local structure, skin structure, mask, UI correction,
  depth, and motion-scale controls.
- New schema-6 configs/presets carry all thirty pass records and the optional
  Clean Fry controls. Complete schema-5 profiles retain every established
  setting and migrate with Clean Fry safely disabled; historical schema-4
  profiles keep their first ten records and passes 11-30 receive safe defaults.
- Performance, Balanced, Quality, Ultra Performance, Ultra Quality, and DLAA
  contracts are normalized after native DLSS so Chicken always works in the
  final output domain. Same-output mode and dynamic-resolution changes can be
  adopted live; output/device changes use a fenced Chicken-only rebuild.
- Smart NGX host discovery supports forwarded exports, late-loaded hosts,
  multiple validated host copies, and cached resolver targets while retaining
  genuine game calls and failing closed on uncertain ownership.
- `arm=0` is a real restart-only hard disarm. It installs no NGX, Streamline,
  loader, or resolver interception.
- Feature graphs, codecs, intermediates, queues, and recreation slots are
  bounded and reclaimed behind D3D12 fence proof to address prior leak-like
  growth and stale performance after disabling/re-enabling.
- Full-resolution and optional half-resolution private neural work are
  available. The game's final output resolution is unchanged.
- The optional Native Look post-process now transfers the complete bounded
  neural luminance result onto the untouched native RGB/tone-mapped base. The
  previous radius-one filter erased most multi-pixel reconstruction and could
  make the option look as though Chicken had been disabled.
- New default-off **Clean Fry** cleanup guards the final result of two or more
  passes against unsupported halos and accumulated colour drift. It uses a
  proxy/native 3x3 range envelope and log-luminance residual limits without
  blurring pixels, changing NGX guides, or resetting histories. One pass and
  disabled mode retain the established decode pipeline exactly.
- Material Lab and the experimental Environment Detail branch are removed from
  the active release to keep setup and troubleshooting predictable.
- A public DLSS5-Feeder ABI, ownership courtesy, explicit synthetic-contract
  marker, genuine-target forwarding, and bounded recreation policy are
  included. See `NOTE-TO-DLSS5-FEEDER-DEVELOPER.md`.

## Confirmed in local testing

| Path | Result |
| --- | --- |
| Starfield, native D3D12 DLSS, RTX 4080 Super, 3840x2160 | DLAA and non-DLAA quality/performance switching confirmed with Chicken active |
| Cyberpunk 2077, native D3D12 DLSS, RTX 4080 Super, 3840x2160 | Three-pass output and the corrected Native Look post-process visually confirmed |
| Automated contract suite | All six native DLSS quality values; 16:9, 16:10, ultrawide, super-ultrawide, and 8K-sized contracts; mode/DRS/output recreation; bounded Feeder and hook policy |
| ReShade | 6.8 full add-on build is the validated baseline |

These are narrow test results, not a promise that every game, driver, GPU, HDR
pipeline, or ReShade update will behave the same way.

## Expected to work, but still needs per-game validation

| Path | Current status |
| --- | --- |
| 64-bit D3D12 game with native DLSS Super Resolution | Primary supported path |
| 64-bit D3D11 game with native DLSS | Use the included `dlss5-dx11-bridge.addon64`; compatibility remains game-specific |
| Game without native DLSS | Use the separate official DLSS5-Feeder transport; the Feeder is not bundled |
| Vulkan/OpenGL or 32-bit game | Feeder transport/companion-host path; upstream maturity varies and community logs are required |
| D3D9 game | Translation-dependent Feeder route through dgVoodoo2/D3D11 |
| SDR/HDR output | Carrier-aware codec is included; validate each game's output because HDR formats and post-DLSS effects vary |

## Known limitations and currently unsupported cases

- **Keep Frame Generation off for this release.** Feature 11 is forwarded and
  never cooked, but current live testing produced a black screen when Frame
  Generation was enabled. Safe topology-transition work remains WIP for a
  later release.
- Ray Reconstruction/DLSSD feature 13 is reported and forwarded untouched.
  Chicken 1.4.0 attaches to DLSS Super Resolution feature 1, not feature 13.
- D3D10 has no direct shipped transport. D3D9 has no native NGX DLSS path and
  depends on the external translation/Feeder route described above.
- Feeder v0.7.0 and v0.8.0-beta.3 are source-contract compatible, but broad
  runtime validation across its D3D11/D3D12/Vulkan/OpenGL and x64-host paths is
  not complete. The 32-bit Vulkan route is especially experimental upstream.
- Native Look preserves the native RGB balance, tone-mapped base, HDR range,
  and alpha while carrying a bounded +/-0.50-stop neural luminance change.
  Equal-luminance neural chroma changes are intentionally discarded, and the
  option cannot undo effects applied after the intercepted DLSS output.
- Passes 11-30 are an extreme stress mode, not a safe default. At 4K, thirty
  passes need about 1.79 GiB for Chicken's twenty-nine intermediates alone, or
  about 459 MiB at half working resolution, before NVIDIA model/history memory.
  They can exhaust GPU time or VRAM and may cause driver resets, black frames,
  or crashes. Start with one pass and raise the count slowly.
- Only the RTX 4080 Super configuration above has been tested by the author.
  Other RTX generations, drivers, and DLSSNR versions are unverified.
- Anti-cheat and protected multiplayer environments are not supported. This
  project does not provide or attempt an anti-cheat bypass.

## Required external files and provider rules

The archive deliberately does **not** include NVIDIA's `nvngx_dlssnr.dll`.
Supply a trusted, signature-verified x64 copy yourself. It also does not bundle
DLSS5-Feeder, RenoDX, OptiScaler, or an NVIDIA model/runtime download.

Run only one neural provider. Remove or rename the separate RenoDX neural
provider (`renodx-dlss5.addon64` or `renodx-dlss.addon64`) before using
Chicken. A game-specific RenoDX HDR add-on may remain for an initial test,
although disabling it temporarily is useful when diagnosing colour problems.

Read `INSTALL-LAYOUTS.md` for exact folder diagrams and `QUICK-START.md` for
setup. When reporting a failure, send `deep-fried-chicken.log`, the matching
`ReShade.log`, game executable/bitness/API, resolution, DLSS mode, GPU, driver,
ReShade version, and DLSSNR file version. Feeder reports also need
`dlss5-feed.log` and the companion host log where applicable.
