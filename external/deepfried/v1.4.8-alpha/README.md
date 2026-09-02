# Deep Fried Chicken 1.4.8 alpha

Experimental standalone ReShade neural-rendering add-on. It intercepts a
game's native D3D12 DLSS evaluation and lets native DLSS finish as a safe
fallback and sole spatial upscaler. It then encodes native DLSS's completed
output-resolution frame and runs DLSSNR as an output-to-output post-pass.
Native render-resolution Depth and Motion Vector resources, scales, and active
subrects remain guide inputs. Optional passes 2 through 30 use the same
output-resolution color domain; no Chicken pass performs a second spatial
upscale.

The owned path attaches only to native NGX feature 1, DLSS Super Resolution.
Ray Reconstruction/DLSSD features are not attached to or altered by this
release. Feature 13 is now reported explicitly in the tab and log so it is not
mistaken for a failed feature-1 interception.

NGX feature 11, Frame Generation, remains game-owned and is forwarded directly.
Chicken never duplicates or recursively cooks generated frames. Version 1.4.7
added a default-off coexistence experiment: after private Chicken work completes,
its base-frame result is copied back on the same native DLSS producer command
list before that list closes. Streamline tags, tag arrays, genuine calls, and
generated frames remain untouched. ReShade command-buffer proxies are locally
unwrapped using the public RenoDX MIT adapter pattern.

Frame Generation now defaults to a **native-safe bypass**. Chicken publishes
the bypass before the genuine `slDLSSGSetOptions` On transition, runs native
DLSS unchanged, and skips its in-place neural post-pass while Streamline owns
presentation. The old synchronous pre-Present queue drain is not armed; it
could wait on work that Streamline itself needed Present to advance and was the
leading black-screen mechanism. After the final FG viewport is disabled,
Chicken waits for one successfully presented native frame and then resumes with
reset temporal history.

The default-off **Experimental: cook base frames with Frame Generation**
checkbox enables that same-producer-list base-frame copyback. Unsupported
producer-list, resource-state, or copy contracts fail open to the native-safe
bypass. This is a title-specific experiment, not general Frame Generation
compatibility; parallel/multi-frame generation, resize, multiple viewports,
and HDR UI recomposition still need live testing.

Version 1.4.8 also removes the recurring 300-Present full-process module scan
from normal steady-state play. Loader/resolver hooks and a process-lifetime
`LdrRegisterDllNotification` callback now publish atomic discovery work for a
later safe callback. Once OS notification registration succeeds, discovery is
event-driven. If registration is unavailable, the 300-Present cadence remains
only during bootstrap; after one native neural path succeeds, the safety scan
slows to once per 3600 presented frames. **Refresh neural contract** explicitly
requests another discovery pass. This directly addresses the reported periodic
1% low candidate, but the affected module-heavy hosts still need A/B confirmation.

This build has no RenoDX or OptiScaler DLSS dependency, provider contract,
shared controls, or fallback. Its compatibility and performance behavior was
implemented independently against public API contracts and observable
behavior; no RenoDX or OptiScaler binary is copied or bundled. A game-specific
RenoDX HDR add-on may coexist, but Deep Fried Chicken does not call into it.
The small source-built
`deep-fried-chicken-nvngx.dll` is a tiny caller-identity bridge for the direct
DLSSNR Init/Create/Evaluate/Release calls required by NVIDIA's caller-module
check; it does not patch NVIDIA code.

Native calls are captured with Microsoft Detours 4.0.1, built from the pinned
official source and statically linked into the add-on. At runtime there is no
extra Detours DLL, download, service, or network access. The corresponding MIT
license is included as `LICENSE-Microsoft-Detours.md`.

The optional D3D11/Vulkan compatibility file is built locally from the
official MIT-licensed NIGos `dlss5-bridge` v1.1.0 source at commit
`1a7a16307af278847b0c551be289e2280ab162ea`. Its upstream and ReShade API
licences and upstream README are included in the package. No quarantined or
downloaded RenoDX binary is redistributed. Games without native DLSS can
instead use the official DLSS5-Feeder as an optional transport; see
`FEEDER-COMPATIBILITY.md`. Feeder is not bundled in this archive.

The original Deep Fried Chicken binaries are copyright (c) 2026 Alexander and
are distributed under `LICENSE-Deep-Fried-Chicken.md`. That licence permits
personal, non-commercial use of unmodified official binaries and sharing
user-created presets, screenshots, and videos. Included third-party components
remain under their separately supplied licences.

The color-preserving tone-transfer codec contains separately identified
RenoDX-derived source portions used under RenoDX's MIT licence. The required
notice is supplied as `LICENSE-RenoDX.md`; no RenoDX binary is included or
required.

`README-dlss5-bridge.md` is retained verbatim as upstream documentation. Its
`src/reshade/LICENSE.md` link therefore does not resolve inside this binary
package; the referenced text is supplied here as `LICENSE-ReShade.md`. Its
generic requirements table also assumes the chosen neural add-on ships
`nvngx_dlssnr.dll`. Deep Fried Chicken does not redistribute NVIDIA's DLL, so
obtain it from a trusted original source and verify its signature/integrity.
Local compatibility testing recorded ProductVersion `310.8.0.0`, but that
version string is evidence only, not a blessed hash or download recommendation.
The experimental local copy currently reports Authenticode `HashMismatch`; it
is deliberately neither hashed in nor supplied by this package.

## Install

Install the full add-on build of ReShade 6.8, the validated baseline. Deep
Fried Chicken registers against exact add-on API 18 and requests exact ImGui
function-table ABI 19250. A newer ReShade build is compatible only while it
still exposes both ABIs; otherwise the add-on fails closed during registration.
Its controls appear in a dedicated `Deep Fried Chicken` ReShade tab. ReShade
may initially open that tab as a floating dockable window when upgrading an
existing saved layout; dock it normally or reset the ReShade layout if desired.
Place these three core files beside the game executable and ReShade proxy
(`dxgi.dll` for the current D3D12 test):

```
deep-fried-chicken.addon64
deep-fried-chicken-nvngx.dll
deep-fried-chicken.cfg
```

The supplied config contains `arm=1`. This restart-only ownership switch must
be enabled before Chicken claims the process-local feature-1 consumer marker
or installs native-NGX, Streamline, loader, or resolver interception. Set
`arm=0` only when another feature-1 consumer must own the process, then fully
restart the game. At `arm=0`, Chicken claims no consumer marker and installs no
native-NGX, Streamline, loader, or resolver interception. The live `enabled`
switch does not release permanent observation hooks and is not a substitute
for `arm=0`.

The game must already initialize native DLSS through a supported NGX host, but
that host's `nvngx_dlss.dll` does not need to be game-local.
`nvngx_dlssnr.dll` is the only NVIDIA component that must sit beside this
add-on. Supply a trusted,
signature-verified copy yourself; this package never supplies any NVIDIA DLL.
For Streamline games such as Starfield, native DLSS is intercepted at the exact
`_nvngx.dll` exports loaded from NVIDIA's Windows driver-store path; a game-local
file using that private name is rejected. Remove
older `alexs-toolkit.addon64`, `dlssnr-cascade*.addon64`, and
`renodx-dlss*.addon64` copies so there is only one neural provider.
The active runtime detects the exact neural-provider filenames
`renodx-dlss5.addon64` and `renodx-dlss.addon64`. If either is loaded, Chicken
latches its neural work off for that process and forwards native/provider
output unchanged. It does not broadly match RenoDX names, so a game-specific
HDR/tone-mapping add-on remains allowed.

This rule also applies to DLSS5-Feeder: its upstream setup normally bundles a
Reno neural provider, but Chicken substitutes for that component. Keep the
Feeder transport and remove the separate Reno neural provider. When both are
detected, the tab and log now report that exact substitution instead of merely
waiting for a feature.

ABI-aware synthetic providers can negotiate explicitly instead of relying on
that exact-name legacy fallback. Chicken exports interop ABI 1 and a current
interception state, claims
`Local\DeepFriedChicken.Feature1Consumer.v1.<decimal process ID>`, and binds a
complete four-key marker to the created feature handle and its hook-host slot.
The four unsigned values are `DFC.Feeder.ContractVersion=1`,
`DFC.Feeder.ProviderId=0x444C3546`, `DFC.Feeder.HostMode=0` in-process or `1`
for the companion x64 host, and `DFC.Feeder.EvaluateCadence=1`. They must be
present immediately before Create and every Evaluate. The complete producer
contract is documented in `FEEDER-INTEROP-v1.md` and summarized in
`FEEDER-COMPATIBILITY.md`.

For a D3D11 game with native DLSS, also place
`dlss5-dx11-bridge.addon64` in that same folder. It mirrors the game's real
D3D11 DLSS contract onto D3D12, including every Performance-through-DLAA
quality mode, so Deep Fried Chicken can receive it. Its settings live in
`dlss5-dx11-bridge.cfg`, which it creates on first launch. Vulkan mirroring is
available through that bridge's documented `vk_mirror=1` setting.

The add-on must be loaded before the game resolves native DLSS. On its first
ordinary safe load, Chicken now adds its exact filename to the existing
`[ADDON] LoadFromDllMain` list in the sibling `ReShade.ini`. Existing entries
and unrelated INI text are preserved, and an existing file receives a sibling
backup before atomic replacement. Fully restart the game once after that
first launch. Malformed, ambiguous, unreadable, or read-only files are left
unchanged and reported in the tab/log.

The resulting setting is:

```ini
[ADDON]
LoadFromDllMain=deep-fried-chicken.addon64
```

If `LoadFromDllMain` already lists another add-on, append
`deep-fried-chicken.addon64` to that comma-separated list instead of replacing
the existing value. The add-on checks this setting before arming at the early
device event; without it, the safe swapchain fallback may be too late for a
game that cached its native DLSS exports during startup.
The dedicated tab also provides the same operation under **Compatibility /
Startup**. Its add/remove button edits only Chicken's exact token, preserves
other entries and INI sections, and clearly requires a full game restart.

The owned provider implements NVIDIA's native quality values Performance,
Balanced, Quality, Ultra Performance, Ultra Quality, and DLAA. It does not
invent a quality mode: render/output dimensions and `PerfQualityValue` are
copied from the game's real NGX call. A missing or out-of-range quality enum, or
a DLAA value inconsistent with the published dimensions, fails closed. Native
DLSS has already completed before added work starts, so an unsupported or failed
added path leaves its output untouched.

DLAA at 3840x2160 on an RTX 4080 Super remains the known-good D3D12 baseline.
For Performance through Ultra Quality, native DLSS first completes the game's
normal render-to-output upscale. Chicken then encodes the complete resolved
output and publishes a 1:1 feature-18 Color/Output contract while retaining
the game's lower-resolution Depth/Motion Vector guide subrects. This replaces
the invalid zero-padded carrier route that produced a small upper-left scene
with a full-resolution HUD. Those quality modes now pass build-time contract
tests but still require wider per-game runtime validation.

The standalone path is dimension-driven and contains no fixed 4K or 16:9
render assumption. Contract tests cover 1280x720, 1920x1080, 2560x1080,
3440x1440, 3840x1600, 5120x1440, 3840x2160, odd-rounded DLSS work extents,
and 7680x4320. Actual availability still depends on the NVIDIA runtime, game
resources, GPU limits, and the exact resource contract below. These tests do
not claim support for every game-specific resource layout.

The provider fails closed if matching Depth or Motion Vectors are unavailable,
if required subrect metadata is inconsistent, or if the D3D12 device/feature
contract is unsupported. The neural output must have a zero X/Y base, its
allocation must exactly match the active output dimensions, and it must support
unordered access. A non-zero output subrect or an oversized output allocation
is not region-aware in this alpha and therefore stays on native DLSS instead
of risking a scaled or upper-left image. Depth and Motion Vector bases are
validated against their allocations. The initial standalone path accepts only
a node-1 creation/visibility mask and fails closed on linked-node devices.

A change between Performance, Balanced, Quality, Ultra Performance, Ultra
Quality, DLAA, or a dynamic-resolution work extent is adopted live when the
output device and output dimensions are unchanged. Chicken updates the guide
contract and performs one temporal-history reset; it does not recreate its
private feature merely because the native input extent or quality enum changed.
If the output dimensions or D3D12 device change, Chicken immediately shows the
native frame, fences every D3D12 queue which submitted Chicken work, reclaims
only Chicken-owned handles/resources, then lazily recreates the graph for the
new output. The game's native feature-1 handle and feature-11 Frame Generation
handle are never released by this rebuild.

Native host discovery is not limited to one filename or one startup scan.
Chicken validates and deduplicates up to eight canonical NGX owner groups,
keeps each feature keyed by `(hook slot, handle)`, and responds to loader,
resolver, module-generation, and `LdrRegisterDllNotification` signals for
late-loaded hosts. Once OS DLL notifications are registered, the old recurring
300-Present full-module scan is disabled. If registration fails, bootstrap
scanning remains at 300 Presents until a native neural path succeeds, then a
3600-Present safety fallback remains for unusual loaders. Forwarder aliases
resolve to their real executable owner, the weakest renamed-export fallback
must be unique, and ambiguous or split ownership fails closed. Resolver
observation never substitutes a Chicken wrapper as the address returned by
`GetProcAddress`. Manual **Refresh neural contract** also requests discovery.

The supplied main cfg starts with `enabled=1`, one pass, no Texture Boost, and
Clean Fry disabled.
The master switch applies live. Disabling first switches to the native game
output, then drains submitted Chicken work behind native D3D12 fences and
reclaims Chicken-owned handles, pair slots, codecs, and retained resources.
Re-enabling lazily creates a fresh graph from the next valid native contract,
so old histories and allocation pressure do not survive an off/on cycle.
Increasing the pass count grows private handles live for the owned provider;
an externally-created legacy pair may still need the game to recreate its
feature. Texture Boost can still require one restart when its separate 8K
handle was not created at feature creation.

The dedicated tab shows the last valid native DLSS mode and its detected
input-to-output resolution, labelled as automatic output sizing. **Refresh
neural contract** performs the same native-only, fenced Chicken rebuild without
requiring a game restart. Use it after a game changes presentation state but
does not publish a new valid evaluation contract.

## Pass controls

`Passes` is the total number of direct DLSSNR evaluations, from one through
thirty. Every `Pass N` panel exposes the same compact neural contract:

- **NR Preset:** Default, Preset #1, Preset #2, or Preset #3;
- **NR Style:** Default, Natural, or Cinematic;
- **NR Intensity, Local Tone Strength, Local Structure Strength, and Skin
  Structure Strength**;
- **Automatic Mask** and **NR UI Correction**;
- **Depth Convention:** use the game's NGX flag, force normal depth, or force
  inverted depth;
- independent **Motion Scale X Multiplier** and **Motion Scale Y Multiplier**;
- **Reset NR feature and clear failure latch**, which schedules a temporal reset and
  recoverable retry safely between wrapped calls. Resetting a private pass also
  resets every downstream neural history; earlier histories stay intact. A fatal device/contract
  latch still requires the game to recreate DLSS or restart.

Pass 1 uses these settings even when `Passes=1`; the standard one-pass path no
longer bypasses its panel. At two to thirty passes, the earlier records are
private pre-passes and the last record is always the final owner. Color
chaining, guide binding, and pass topology are fixed to the validated safe
path rather than exposed as experimental user controls.

The first neural pass reads an encoded proxy of the completed native-DLSS
output.
The same output codec preserves that frame as the fallback and HDR
reconstruction context. HDR is taken from
`DLSS.Feature.Create.Flags` together with the actual output carrier format.
The linear-HDR codec is selected only when the flag is set and the carrier is
a validated floating-point HDR format; UNORM/PQ-compatible carriers remain on
the non-linear-safe path instead of being mis-decoded and washed out.
Scene-linear colour is soft-clipped and encoded to sRGB for DLSSNR, then
decoded and tone-upgraded while reconciling neural luminance with the native
output context and preserving hue and alpha. The
baseline uses Paper White Scale `1.765`,
Transfer Strength `1.0`, Color Strength `1.0`, the Default preset/style, and
the retained known-good NR strengths of `2.0`. Depth and Motion Vectors are
guides, not outputs, so they are never cascaded, fabricated, or rewritten.
Every temporary parameter change is restored and verified before returning to
the game. If restoration or a private configuration fails, the add-on disables
that pair's extra path and records the reason in the log.

The standalone provider never creates an unequal-resolution feature-18 owner;
all of its private and final passes are 1:1 in the resolved output domain. The
older zero-padded owner-carrier implementation remains isolated only for legacy
external feature-18 contracts and is unreachable from the standalone path.

The supplied schema-6 cfg preserves the golden one-pass baseline: enabled,
`Passes=1`, Pass 1 Default preset and Default style, all four NR strengths at
`2.0`, Automatic Mask and UI Correction off, game NGX depth convention, and
Motion Scale X/Y at `1.0`. Native Look, Texture Boost, and Clean Fry start off.
The shared color-transfer defaults are Paper White `1.765`, HDR Transfer `1.0`,
and Color Strength `1.0`.

## Shared Control-compatible color transfer

**Scene Paper-White Scale**, **HDR Transfer Strength**, and **Color Strength**
are one shared section, not duplicated under each pass. They control the real
output codec which encodes the completed native frame before Pass 1 and decodes
the result after the final pass, so they wrap the chain as a single
Control-compatible color-transfer boundary. Treating them as independent
per-pass sliders would imply repeated color-space conversions which the
pipeline does not perform.

Material Lab and the experimental Environment Detail branch are removed from
the active 1.4.8 release. Their old config/preset keys are ignored during
migration and cannot reactivate either path.

## Native Look detail-only output

`Post-process: restore native tone & color, keep neural detail` is an optional final-composite
mode, off by default. The output codec already retains the exact native DLSS
frame and a matching neural-domain proxy before any added passes. The mode
compares the final neural luminance with that exact proxy, clamps the relative
change to +/-0.50 stops, and applies it as a scalar gain to the untouched
native frame. Native RGB ratios, the tone-mapped base, HDR range, and alpha
therefore remain owned by the game while coherent neural reconstruction and
fine contrast remain visible. Earlier builds high-passed this gain over a
radius-one neighbourhood, which erased most multi-pixel face and surface
changes and made the option look like a neural bypass.

This is not an inverse of the game's tone mapper and it does not recover effects
applied after the intercepted DLSS output. Broad neural luminance changes inside
the safety bound remain part of the result; equal-luminance chroma changes are
deliberately discarded so native colour balance stays stable. The branch
allocates no resource, changes no temporal input, and requires no history reset
or restart. With the option off, the existing SDR and HDR decode paths are
unchanged.

## Clean Fry multi-pass cleanup

`Clean Fry` is an optional final spatial guard for two or more
neural passes. It compares the final neural result with both the exact proxy
fed into the chain and the untouched native frame over a clamped 3x3
neighbourhood. A YCoCg range envelope limits unsupported luminance/chroma
excursions, while a log-luminance residual test keeps locally supported fine
detail and suppresses broad halos or accumulated colour drift.

The cleaned pixel can only move along that pixel's original proxy-to-neural
result; neighbouring pixels define safety limits but are never averaged into
the output. Clean Fry therefore adds no blur buffer, does not modify DLSS
guides or temporal histories, and does not recreate neural features when its
controls change. **Cleanup Strength** controls how strongly the guard is
applied. **Detail Retention** controls how much source-supported high-frequency
change survives.

Clean Fry is disabled by default and is an exact dry bypass at one pass. When
disabled, unavailable, or given a zero cleanup strength, the established
11-constant decode shader and pipeline state are used unchanged. This first
version is intentionally spatial: it can reduce per-frame halo/ringing and
colour buildup, but it cannot fully repair temporal ghosting already produced
inside the neural cascade.

## Shareable per-game presets

The main `deep-fried-chicken.cfg` persists every control: requested pass count,
Native Look output, Texture Boost and strength, Clean Fry settings, the shared
color-transfer settings, and the complete neural settings for all thirty pass
records. Dormant-pass values are saved too.

The dedicated ReShade tab also has `Export <game>.cfg` and `Load <game>.cfg`
buttons.
Export creates a complete preset beside the add-on at:

```
<folder containing deep-fried-chicken.addon64>\deep-fried-chicken-presets\<game executable name>.cfg
```

For example, Starfield exports to
`deep-fried-chicken-presets\Starfield.cfg`. Share that one file. A recipient
places it in the same subfolder beside their copy of the add-on and presses
`Load Starfield.cfg`. If their executable has a different filename, rename the
preset to match the executable name shown on the Load button.

Load validates and clamps the file, then publishes the complete profile only
between wrapped NGX calls. Active temporal histories are reset when required.
If the game is busy, no partial profile is applied; press Load again. The
startup `enabled` switch is intentionally not exported or loaded. Texture Boost
may still require one restart if its private handle was not armed when the game
created DLSSNR; all already-created pass controls load live.

New exports use `preset_schema=6` and contain every simplified pass record plus
the shared chain settings and complete Clean Fry block. Complete schema-1,
schema-2, schema-3, historical ten-record schema-4, and complete schema-5 presets
remain loadable: recognized neural, pass-count, Native Look, and Texture Boost
values are validated and clamped, controls absent from those legacy schemas
receive safe golden defaults, and removed Material Lab and Environment Detail
keys are ignored. Passes 11 through 30 receive golden defaults when importing
schema 4. Schema 5 preserves all established settings and migrates with Clean
Fry disabled at its safe numeric defaults. An incomplete schema-6 preset is
rejected as a whole rather than partially applied. Import does not rewrite the
shared legacy preset. The next explicit export writes a clean schema-6 file,
so migration is repeatable and reversible.

## Memory costs

Each active private 3840x2160 RGBA16F pass intermediate is approximately
63.28 MiB. Nine private intermediates (the former ten-pass ceiling) consume
about 569.53 MiB; twenty-nine at the new thirty-pass ceiling consume about
1,835.16 MiB before NVIDIA's model/history allocations. Half working
resolution reduces the twenty-nine-intermediate payload to about 458.79 MiB;
Quarter reduces it to about 114.70 MiB.
Texture Boost can add a further 506.25 MiB. Passes 11 through 30 are explicitly
an extreme VRAM/TDR stress mode, not a recommended gameplay setting.

The baseline output codec retains two native-format images and two FP16 images.
At 3840x2160 this is about 189.84 MiB when the native resources are 32-bit, or
253.13 MiB when they are FP16. Performance-through-Quality modes now use this
same output-resolution codec and do not allocate a second render-sized input
codec or owner carrier. The allocation guard uses D3D12's exact committed-size
preflight rather than these payload-only estimates.

Within one active graph, codec contracts remain capped at four and 1 GiB of
codec-owned committed allocations. Master disable, manual refresh, and an
output/device change reclaim that graph only after its tracked D3D12 queues
cross native fences. If queue tracking or an NGX release becomes uncertain,
the add-on fails closed and retains the resources until process exit rather
than risk freeing memory still referenced by the GPU.

One pass allocates no private cascade handles or intermediates.

## Compatibility and migration

Current API boundary:

| Game API | Status | Quality range |
| --- | --- | --- |
| D3D12 with native DLSS Super Resolution | Built in; same-output mode/DRS changes apply live; output/device changes use a fenced Chicken-only rebuild; Streamline resolver, setter, `slSetTag`, and `slSetTagForFrame` entries are observed. FG defaults to native-safe bypass with no blocking pre-Present drain; feature 11 and Ray Reconstruction/DLSSD feature 13 remain game-owned and forwarded untouched | DLAA validated at 4K/RTX 4080 Super; Performance through Ultra Quality and 16:9/ultrawide/super-ultrawide contracts tested. FG-safe bypass is automated; same-producer-list base-frame copyback remains experimental and requires per-game validation |
| D3D11 with native DLSS | Optional official bridge v1.1.0; all presets tested by upstream in Baldur's Gate 3 | Performance through DLAA |
| Vulkan with native DLSS | Experimental upstream `vk_mirror=1`; upstream reports no Vulkan-title run yet | Mirrored game contract, unvalidated in a Vulkan title |
| D3D11/D3D12 game without native DLSS | Optional official DLSS5-Feeder synthetic contract | Feeder DLAA/work-resolution contract; source compatible, runtime validation required |
| Vulkan/OpenGL game without native DLSS | Optional official DLSS5-Feeder cross-API transport | Feeder synthetic DLAA contract; upstream support maturity varies |
| 32-bit D3D11/OpenGL and experimental Vulkan | Optional Feeder x86 add-on plus x64 host; Chicken runs inside `host64` | Feeder synthetic DLAA contract; version-matched Feeder package required |
| D3D9 | Optional dgVoodoo2-to-D3D11 path documented by Feeder | Feeder synthetic DLAA contract; translation-dependent |
| D3D10 | No direct transport; a separate translation route is required | None directly |

The optional **Neural work scale** slider keeps the display/output contract
unchanged while running Chicken's complete feature-18 graph at any whole
percentage from 10% to 150%. Work dimensions are checked and rounded up as
`ceil(display * percent / 100)`. Exactly 100% retains the bit-exact native-size
codec path. Values below 100% reduce working pixels and trade neural detail and
temporal stability for performance; values above 100% are experimental neural
supersampling and can sharply increase GPU time and VRAM. A scale change uses
the fenced Chicken-only recreation path. Unsupported dimensions or allocation
budgets fail closed to the native frame instead of cropping or stretching it.
The cfg and exported presets also retain a canonical
`neural_work_divisor=1|2|4` rollback hint so a v1.4.3 binary can safely ignore
the new percent key.

D3D9 and D3D10 do not have native NGX DLSS APIs. Merely accepting those
ReShade backend enum values would not supply Color, Depth, Motion Vectors,
jitter, or a lower-resolution render target, so this build does not advertise
false support. The adapter design is documented in
`LEGACY-API-ADAPTER.md`; implementation requires explicit shared
textures and synchronization across two API boundaries. The optional Feeder
route supplies such a synthetic boundary for the APIs it supports and is
documented separately in `FEEDER-COMPATIBILITY.md`.

Configuration lookup order is:

1. `deep-fried-chicken.cfg`
2. `alexs-toolkit.cfg`
3. `dlssnr-cascade.cfg`

When a legacy file is found it is copied atomically to the new filename and
the source is retained. Legacy `two_pass`/`three_pass` settings map to one, two,
or three passes. Supported neural values migrate into the corresponding pass;
removed blend, material, and environment experiments are ignored. The copied
source is never deleted, so restoring the previous package remains the
rollback path.

No external neural-provider contract is required by the core. Smart discovery
examines trusted driver, observed-call, known-leaf, and unique-export evidence;
validates executable export ownership; pins every selected owner; and
persistently detours its D3D12 Create/Evaluate/Release entry points. The proven
native host names (`_nvngx.dll`, `nvngx.dll`, `nvngx_dlss.dll`, and
`nvngx_dlssd.dll`) remain known leaves, but exact filename alone is not treated
as proof. A private `_nvngx.dll` name is accepted only from NVIDIA's Windows
DriverStore path. The original functions always run first through Detours
trampolines. The add-on then owns its feature-18 handles and parameter map and
loads only the exact sibling `nvngx_dlssnr.dll` plus its source-built call
bridge.

## Build

Run `build.cmd` from a normal command prompt. It produces:

```
bin\deep-fried-chicken.addon64
bin\deep-fried-chicken-nvngx.dll
bin\deep-fried-chicken.cfg
..\..\release\v1.4.8-alpha\deep-fried-chicken.addon64
..\..\release\v1.4.8-alpha\deep-fried-chicken-nvngx.dll
..\..\release\v1.4.8-alpha\deep-fried-chicken.cfg
..\..\release\v1.4.8-alpha\dlss5-dx11-bridge.addon64
..\..\release\v1.4.8-alpha\README.md
..\..\release\v1.4.8-alpha\QUICK-START.md
..\..\release\v1.4.8-alpha\COMPATIBILITY-TEST.md
..\..\release\v1.4.8-alpha\FEEDER-COMPATIBILITY.md
..\..\release\v1.4.8-alpha\FEEDER-INTEROP-v1.md
..\..\release\v1.4.8-alpha\LICENSE-Deep-Fried-Chicken.md
..\..\release\v1.4.8-alpha\LICENSE-RenoDX.md
..\..\release\v1.4.8-alpha\LEGACY-API-ADAPTER.md
..\..\release\v1.4.8-alpha\LICENSE-Microsoft-Detours.md
..\..\release\v1.4.8-alpha\LICENSE-NIGos-dlss5-bridge.md
..\..\release\v1.4.8-alpha\LICENSE-ReShade.md
..\..\release\v1.4.8-alpha\LICENSE-dlss5-bridge-vulkan-shader.md
..\..\release\v1.4.8-alpha\README-dlss5-bridge.md
..\..\release\v1.4.8-alpha\SHA256SUMS.txt
```

Before staging anything, the build must pass all supplied smoke tests and
rebuild the official bridge from a Git-clean copy of the exact pinned commit.
It also verifies that the Feeder compatibility analysis still points at the
Git-clean v0.8.0-beta.3 commit named in `FEEDER-COMPATIBILITY.md`; no Feeder
binary or source is copied into the package. Git is required for those
provenance checks. The release directory is refreshed through an explicit
filename allowlist; an unexpected file or subdirectory stops the build instead
of being deleted or silently packaged.

## Verify a release download

Compare the downloaded archive with the SHA-256 value published alongside the
release. In PowerShell, replace the filename if necessary:

```powershell
(Get-FileHash .\Deep-Fried-Chicken-v1.4.8-alpha.zip -Algorithm SHA256).Hash
```

After extracting, `SHA256SUMS.txt` records the expected hashes for the three
shipped binaries. This PowerShell check fails if any one of them differs:

```powershell
Get-Content .\SHA256SUMS.txt |
  Where-Object { $_ -match '^[0-9A-Fa-f]{64}\s+' } |
  ForEach-Object {
    $expected, $name = $_ -split '\s+', 2
    $actual = (Get-FileHash -LiteralPath $name.Trim() -Algorithm SHA256).Hash
    if ($actual -ne $expected.ToUpper()) { throw "Hash mismatch: $name" }
  }
```

Run `compat\build-official-dx11-bridge.cmd` to rebuild the pinned upstream
bridge independently. The main build packages the resulting locally-built
binary and its notices. The separate Texture Boost path remains fixed
3840x2160 to 7680x4320, is labelled as such in the UI, and is unrelated to the
general Performance-through-DLAA owner contract. Unsupported resolutions
cannot enter that path and remain on the normal neural chain. Old releases and
the `cascade-prototype` source tree are preserved.
