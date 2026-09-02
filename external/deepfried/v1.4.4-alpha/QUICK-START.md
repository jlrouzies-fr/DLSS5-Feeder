# Deep Fried Chicken 1.4.4 alpha - Quick Start

Deep Fried Chicken is an experimental x64 ReShade add-on that adds one to thirty
DLSS Neural Rendering passes after a game's native DLSS Super Resolution call.
It is extremely GPU/VRAM intensive. Back up the game folder, start with one
pass, and expect compatibility and image quality to vary by game.

Use of the original binaries is governed by
`LICENSE-Deep-Fried-Chicken.md`. Share the author's official release link
rather than rehosting or repackaging the archive. User-created presets,
screenshots, videos, and reviews may be shared.

## Requirements

- Windows and an NVIDIA RTX GPU. The core path needs a 64-bit game with native
  DLSS Super Resolution. Games without native DLSS, cross-API games, and
  32-bit games can use the optional official DLSS5-Feeder transport described
  in `FEEDER-COMPATIBILITY.md`. The known-good core baseline is Starfield at
  3840x2160 DLAA on an RTX 4080 Super.
- ReShade 6.8 **with full add-on support**, installed beside the real game
  executable rather than a launcher.
- A trusted, signature-verified `nvngx_dlssnr.dll` beside this add-on. NVIDIA's
  DLL is not included or redistributed by Deep Fried Chicken. The locally
  tested ProductVersion was `310.8.0.0`; other DLSSNR builds are currently
  unverified.

Compatibility and performance behavior was implemented independently against
public contracts and observable behavior. No RenoDX or OptiScaler binary is
copied or bundled. Separately identified RenoDX-derived color-codec source is
used under its MIT licence and notice.

## Remove competing neural add-ons first

Do not run two DLSS/neural providers together. Remove or temporarily rename:

```text
renodx-dlss5.addon64
renodx-dlss.addon64
alexs-toolkit.addon64
dlssnr-cascade*.addon64
```

A separate, game-specific RenoDX HDR/tone-mapping add-on may remain because
Deep Fried Chicken does not depend on or replace it. If a game crashes or the
image is wrong, test once with that HDR add-on disabled too, then re-enable it
after Deep Fried Chicken works by itself.

## D3D12 installation

Copy these files from the archive into the folder containing the actual game
`.exe` and ReShade proxy, not the Steam/EA/Rockstar launcher folder:

```text
deep-fried-chicken.addon64
deep-fried-chicken-nvngx.dll
deep-fried-chicken.cfg
```

The release config starts with `arm=1`. This restart-only hard gate lets
Chicken claim feature-1 ownership and install its NGX/Streamline observation
hooks. Set `arm=0` before launch when another feature-1 consumer must own the
process; a full game restart is required in either direction. The live
`Enabled` checkbox controls Chicken's owned neural graph but does not disarm or
remove permanent observation hooks.

Place your trusted `nvngx_dlssnr.dll` in that same folder. Do not replace the
game's existing `nvngx_dlss.dll` for the first test.

Chicken now performs this step automatically on its first ordinary safe load.
It preserves the existing list and unrelated INI text, then asks for one full
game restart. The resulting `ReShade.ini` entry is:

```ini
[ADDON]
LoadFromDllMain=deep-fried-chicken.addon64
```

If `LoadFromDllMain` already contains another filename, append
`deep-fried-chicken.addon64` to its comma-separated list. Do not erase the
existing entries.

If the automatic edit is blocked by a malformed, duplicate, read-only, or
custom-location INI, open **Deep Fried Chicken > Compatibility / Startup**,
read the reported state, or add the line manually. The button adds only this
add-on and preserves other early-load entries. A full restart is required.

## D3D11 installation

Follow the D3D12 steps, then also copy:

```text
dlss5-dx11-bridge.addon64
```

The bridge creates `dlss5-dx11-bridge.cfg` on first launch. It is only for
D3D11 games that already have native DLSS; it does not add DLSS to games that
never supported it.

## Games without native DLSS, Vulkan/OpenGL, D3D9, and 32-bit

Use the official DLSS5-Feeder package as the transport. Keep Feeder, remove
the separate `renodx-dlss5.addon64` neural provider, and place Chicken on the
64-bit side of the pipeline. For a 32-bit game that means inside Feeder's
`host64` folder, not beside the x86 executable. Do not combine Feeder with the
included native-DLSS DX11 bridge.

Feeder's own normal instructions include Reno because it needs a neural
provider; Chicken replaces that component for this setup. If both are present,
the Chicken tab will identify the exact conflict and remain safely inert until
Reno is removed and the process is fully restarted.

Follow `FEEDER-COMPATIBILITY.md` for the exact layouts, version-matching rule,
ABI-1 negotiation markers, `warmup_rebuild=0` recommendation, API matrix, and
required logs. ABI-aware Feeder builds publish all four `DFC.Feeder.*` keys at
Create and every Evaluate; older exact upstream filenames use a deliberately
narrow legacy fallback.

## First launch

1. Enable DLSS or DLAA in the game's own settings before judging the add-on.
2. Open ReShade and select the dedicated **Deep Fried Chicken** tab.
3. Check **Compatibility / Startup**. On a first ordinary load, Chicken should
   report that it registered early loading for the next launch. Fully restart
   before troubleshooting a `waiting for native DLSS` status.
4. Confirm **Arm feature-1 interception on startup** is enabled. Changing it
   saves `arm=1` or `arm=0` for the next full process start; it is not a live
   enable/disable control.
5. Start with `Enabled` on and `Passes` set to `1`.
6. Leave Pass 1 at the golden defaults for the first run: NR Preset/Style
   `Default`, all four strength sliders `2.0`, Automatic Mask and UI Correction
   off, Depth Convention `Use game NGX flag`, and Motion Scale X/Y `1.0`.
7. Check the `Last valid` line. It shows the detected native DLSS mode and
   input-to-output resolution; output sizing is automatic. If the game has
   changed presentation state without publishing a fresh valid contract, press
   **Refresh neural contract** and let the native-only fenced rebuild finish.
8. Increase passes one at a time. Passes 11 through 30 are an extreme stress
   mode. High counts can cause massive FPS loss,
   VRAM exhaustion, driver resets, black frames, or crashes.
9. Optional: enable **Post-process: restore native tone & color, keep neural detail** to keep the
   game's RGB balance, tone-mapped base, HDR range, and alpha while transferring
   the complete bounded neural luminance result from the added passes.
10. Optional: at two or more passes, open **Clean Fry** and
    enable its final residual cleanup. Start around Cleanup Strength `0.35` and
    Detail Retention `0.85`, then raise cleanup slowly while watching fine text,
    hair, foliage, and moving edges. It is automatically bypassed at one pass.

Use **Export <game>.cfg** and **Load <game>.cfg** in the tab to share complete
per-game presets. Presets are stored in `deep-fried-chicken-presets` beside the
add-on. New exports use schema 6. Schema-1, schema-2, schema-3, historical
ten-record schema-4, and complete schema-5 presets load
safely: supported values migrate, controls absent from those legacy schemas
receive golden defaults, and removed Material Lab/Environment Detail keys are
ignored. Passes 11 through 30 receive golden defaults when schema 4 is loaded.
Schema 5 retains all prior settings and starts Clean Fry dry. An incomplete
schema-6 preset is rejected without applying any of it. Loading does not
rewrite the old preset; exporting writes a new clean schema-6 file.

## Controls

Every pass from 1 through 30 has the same NR Preset, NR Style
(Default/Natural/Cinematic), NR Intensity, Local Tone, Local Structure, Skin
Structure, Automatic Mask, UI Correction, depth convention, and independent
Motion Scale X/Y controls. Pass 1 settings apply in the normal one-pass mode.
The reset/retry button schedules a safe temporal reset between DLSS calls and
retries recoverable failures; a fatal device/contract failure still needs a
native DLSS recreation or full restart.

The main `Enabled` switch is a graph-lifetime control, not only a visual bypass.
Turning it off first restores native game output, then drains submitted Chicken
work behind D3D12 fences and reclaims Chicken-owned handles and resources.
Turning it on lazily creates a fresh graph from the next valid DLSS contract.

Scene Paper-White Scale, HDR Transfer Strength, and Color Strength appear once
in the shared Control-compatible color-transfer section. They wrap the entire
pass chain and are not per-pass effects. Texture Boost remains separate and is
off by default. Its experimental path accepts only an exact 3840x2160 neural
output and produces an 8K intermediate; other resolutions stay on the normal
pass chain without Texture Boost. Enabling it can still require one restart if
its private handle was not armed when DLSSNR was created.

Clean Fry is a separate default-off final cleanup for multi-pass output. It
bounds unsupported halo and colour buildup without averaging neighbouring
pixels, changing guides, or resetting temporal histories. Cleanup Strength sets
how strict it is; Detail Retention keeps more locally supported fine structure.
Zero strength, disabled, unavailable, and one-pass modes use the established
decode path unchanged.

**Neural work scale** defaults to 100%. Its slider accepts 10% through 150%
and reconstructs to the unchanged display extent. Work dimensions are
`ceil(display * percent / 100)`: lower values trade detail and temporal
stability for performance, while values above 100% are experimental neural
supersampling with much higher GPU/VRAM cost. Exactly 100% keeps the original
native-size path. Apply scale changes from the tab; Chicken performs a fenced
graph recreation. If a requested size exceeds the device/allocation limit it
fails closed to the native game frame.

HDR codec choice uses both the game's NGX HDR flag and the actual output
carrier. Only validated floating-point carriers enter the linear-HDR path;
UNORM/PQ-compatible carriers use the non-linear-safe path to avoid a washed-out
or crushed frame.

## Older DLSS versions

- Keep the game's original `nvngx_dlss.dll` initially. Deep Fried Chicken does
  not replace it and has no simple "DLSS 2/3/4" minimum: it observes the
  game's native NGX DLSS Super Resolution call and validates the actual
  resources and metadata at runtime.
- Older D3D12 DLSS games may work when they publish compatible Color, Depth,
  Motion Vectors, dimensions, jitter, and quality metadata. If they do not,
  the add-on fails closed and leaves the game's native DLSS frame untouched.
- Performance through Ultra Quality are normalized after native DLSS: the
  game performs its usual spatial upscale first, then every Chicken pass runs
  output-resolution to output-resolution. Render-resolution Depth and Motion
  Vector guide subrects remain unchanged. This prevents the former small
  top-left scene/quadrant failure seen at non-DLAA quality modes.
- Frame Generation is not a Chicken quality mode. NGX feature 11 is forwarded
  untouched, and Chicken processes only the native DLSS base frames. The tab
  reports when Frame Generation has been observed. Frame Generation remains
  experimental compatibility work: establish a stable FG-off baseline, then
  test FG separately for that game and collect logs if it black-screens or
  stalls.
- An old `nvngx_dlss.dll` is not a substitute for `nvngx_dlssnr.dll`. The
  separate DLSSNR runtime is mandatory, and versions other than the locally
  tested `310.8.0.0` are unverified in this alpha.
- D3D11 with native DLSS needs the included bridge. Games without native DLSS,
  D3D9 translation, Vulkan/OpenGL, and 32-bit hosts need the optional official
  DLSS5-Feeder route. D3D10 has no direct shipped transport. Ray
  Reconstruction-only interception remains unsupported.

## 1.4.4 compatibility changes

- Replaces the fixed Full/Half/Quarter selector with a continuous 10-150%
  Neural Work Scale slider. Values above 100% supersample the neural pass;
  display resolution is never changed.
- Reads old `neural_work_divisor=1|2|4` configs as 100/50/25% and writes a
  canonical divisor alongside the new percentage so rollback remains safe.

- Adds a restart-only `arm=1`/`arm=0` ownership gate. `arm=0` claims no
  process-local consumer mutex and installs no native-NGX, Streamline, loader,
  or resolver interception.
- Adds smart multi-host discovery. Loaded NGX surfaces are validated by owner
  and executable target, forwarder aliases are deduplicated, up to eight
  canonical owner groups can be tracked, and each feature remains keyed to its
  exact hook slot and handle. Discovery continues for late-loaded modules;
  ambiguous weak matches fail closed and genuine `GetProcAddress` results are
  never replaced.
- Adds explicit Feeder ABI 1 negotiation. A complete four-key marker is bound
  to Create and must match every Evaluate, with one synthetic Evaluate per
  Present. Chicken exports its ABI/state and uses the PID-suffixed
  `Local\DeepFriedChicken.Feature1Consumer.v1.<pid>` consumer mutex. Partial or
  unknown markers fail closed; exact older Feeder identities retain a narrow
  legacy fallback.
- Selects the linear-HDR codec only for a flagged floating-point carrier,
  preserving the safer non-linear path for UNORM/PQ-compatible outputs.
- Keeps working-resolution, output-size, and device changes on the fenced
  Chicken-only recreation path; same-output native quality/DRS changes remain
  live adoptions.
- Automatically registers `deep-fried-chicken.addon64` in the sibling
  `ReShade.ini` early-load list on the first safe ordinary load, preserving
  existing entries and requiring one full restart.
- Graduates **Clean Fry** from its Experimental menu label; its default-off
  behavior and compatible `clean_fry_*` settings are unchanged.

- Keeps Quality, Balanced, Performance, Ultra Performance, and Ultra Quality
  active after their first frame when a game omits unused native Color or
  already-resolved jitter fields from later DLSS Evaluate maps.
- Catches the exact dynamic
  `GetProcAddress(sl.interposer.dll, "slGetFeatureFunction")` route plus
  process-lifetime resolver/setter entry detours when Streamline cached
  `slGetFeatureFunction` or the resolved feature-1000 `slDLSSGSetOptions`
  before Chicken loaded. Only that exact setter is wrapped. Per-viewport mode,
  multiplier, buffer, and allocation-topology changes gate Chicken before the
  genuine setter and topology Present without altering arguments or results.
- At the next pre-Present boundary, proves submitted Chicken command lists,
  fences their D3D12 queues, and detaches Chicken's queue references before
  Streamline rebuilds presentation. After the replacement Present, terminal
  Reset/destroy proof plus a final queue fence permits graph reclamation and
  lazy neural rearm. Failure keeps the graph quarantined and requires restart.
  Broad Frame Generation support remains WIP and needs live validation across
  games.
- For games that tear down presentation before calling `slDLSSGSetOptions`,
  the last-presented primary swapchain's `destroy_swapchain` callback supplies
  the same pre-destruction fence and queue-detach boundary. Probe or unrelated
  swapchains are ignored; reclamation and rearm still wait for replacement
  finish-Present proof.

- Fixes the small upper-left 3D scene with a normal full-size HUD in games
  using Performance, Balanced, Quality, Ultra Performance, or Ultra Quality.
  The runtime uses the game's actual output dimensions rather than a fixed
  resolution/aspect ratio; common 16:9, ultrawide, super-ultrawide, odd-rounded
  DLSS work extents, and 8K contracts are covered by contract tests.
- Adopts a switch among all six native modes, or a dynamic-resolution work
  extent, live when the output device and output dimensions stay the same. The
  guide contract changes and histories reset once; the private neural feature
  is not recreated for an input-size or quality-enum change.
- When the output dimensions or D3D12 device change, immediately falls back to
  the native frame and performs an automatic fenced Chicken-only rebuild. The
  game-owned DLSS and Frame Generation features are not released.
- Makes disable/re-enable a clean lifecycle: disable drains and reclaims the
  Chicken graph, and re-enable recreates it lazily instead of keeping stale
  histories and retained allocations. **Refresh neural contract** exposes the
  same safe rebuild manually.
- Shows the last valid detected mode and input-to-output resolution in the
  dedicated tab, and reports Frame Generation feature 11 while forwarding it
  untouched. Chicken cooks base frames only.
- Detects the exact competing neural providers `renodx-dlss5.addon64` and
  `renodx-dlss.addon64`, stops Chicken neural work, and leaves native/provider
  output active. Remove the competing file and restart. Unrelated game-specific
  RenoDX HDR add-ons are not blocked.
- Reports Ray Reconstruction/DLSSD feature 13 explicitly. It is forwarded
  untouched; select DLSS Super Resolution feature 1 in any supported native
  quality mode to use Chicken.
- Adds the reversible early-load setup control described above.
- Recognizes DLSS5-Feeder as a compatible synthetic feature-1 transport,
  including its x64 host process, while continuing to block a second neural
  provider. See `FEEDER-COMPATIBILITY.md` for the honest validation boundary.
- Requires a zero-base neural output allocation whose dimensions exactly match
  the active output and which supports unordered access. Non-zero output
  subrects, oversized output allocations, linked-node devices, and other
  unvalidated resource layouts fail closed to native DLSS; this alpha does not
  claim universal game/resource-layout compatibility.
- Simplifies every pass to the same Reno-style neural controls, makes Pass 1
  obey them in one-pass mode, and moves the real color-transfer controls into
  one shared chain section. Material Lab and Environment Detail are removed
  from the active release.

## If it does not work

Close the game before changing DLL/add-on files. Test with one pass at the
golden defaults, Texture Boost off, Native Look off, and no competing neural
provider. Then send:

- game name and exact executable;
- rendering API and in-game DLSS mode;
- GPU and driver version;
- ReShade version;
- `deep-fried-chicken.log` and the relevant ReShade log lines.

Stay in the menu or gameplay for at least 60 seconds before collecting logs;
startup-only logs that end on an 8x8 probe swapchain do not prove whether the
real game DLSS evaluation was reached. Include a screenshot of the full
Chicken tab so its exact status line is visible.

See `README.md` for the full compatibility contract, known limits, memory
costs, preset format, bridge provenance, and SHA-256 verification details.
