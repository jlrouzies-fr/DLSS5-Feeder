# Deep Fried Chicken + DLSS5-Feeder

Deep Fried Chicken 1.4.0 can consume the synthetic DLAA feature-1 contract
created by DLSS5-Feeder. This extends Chicken to games which do not make native
DLSS calls without embedding or reimplementing Feeder's API transports.

The compatibility target is the official DLSS5-Feeder v0.7.0 release and the
current v0.8.0-beta.3 source at commit
`a96cd72296f4a2d1552d8ef7877300f4a19ebfda`. The 0.8 build is a public test
build, and its 32-bit Vulkan route has not yet been proven in a real game by
upstream. Download Feeder only from its official release page:

https://github.com/jlrouzies-fr/DLSS5-Feeder/releases

Feeder is an optional transport and is not included in the Chicken archive.
Its own files, shaders, motion-vector provider, ReShade setup, and version-
matched 32-bit host remain governed by its upstream instructions and licence.

Deep Fried Chicken also does not bundle NVIDIA's `nvngx_dlssnr.dll`; the user
must supply a trusted, signature-verified x64 copy on the same side of the
transport as Chicken.

## ABI 1 negotiation and ownership

An ABI-aware Feeder should discover the exported unsigned
`DFC_FeederInteropAbi` value `1` and read the exported
`DFC_Feature1InterceptionState`. `CLAIMING` or `ARMED` means Chicken is
available for new synthetic feature-1 work; `DISARMED`, `CONFLICT`, or `FAILED`
means it is not. Feeder must not write either export.

Chicken uses this first-comer process-local consumer mutex:

```text
Local\DeepFriedChicken.Feature1Consumer.v1.<decimal process ID>
```

Feeder is a contract producer and must not claim the mutex. Chicken's main
config `arm=0` is a restart-only hard disarm: it claims no mutex and installs
no native-NGX, Streamline, loader, or resolver interception. Use `arm=1` and a
full restart for normal Feeder operation. The live `enabled=0` switch only
drains Chicken-owned neural work; it does not disarm permanent observation.

Immediately before feature-1 Create and again before every feature-1 Evaluate,
publish all four unsigned values on the shared NGX parameter object:

| Key | ABI 1 value |
| --- | --- |
| `DFC.Feeder.ContractVersion` | `1` |
| `DFC.Feeder.ProviderId` | `0x444C3546` (`DL5F`) |
| `DFC.Feeder.HostMode` | `0` in-process, `1` companion x64 host |
| `DFC.Feeder.EvaluateCadence` | `1` (one Evaluate per Present) |

The tuple is atomic and is bound to the successfully created feature handle
and its exact smart-hook host slot. A partial tuple, unknown value, or
Create/Evaluate mismatch grants no Feeder exception; Chicken leaves the
producer/native output active. An unmarked native Create cannot be converted
into a Feeder handle later at Evaluate.

Published Feeder releases which predate these keys can still use Chicken's
narrow legacy fallback for the exact upstream add-on/host identities. That is
compatibility debt, not a generic filename heuristic. The negotiated path
becomes active when the Feeder build publishes the complete ABI-1 tuple. See
`FEEDER-INTEROP-v1.md` for the stable producer contract.

## One neural provider only

When using Feeder with Chicken, keep Feeder but remove the separate RenoDX
neural provider:

```text
KEEP    dlss5-feed.addon64       (64-bit game)
KEEP    dlss5-feed.addon32       (32-bit game)
REMOVE  renodx-dlss5.addon64
REMOVE  renodx-dlss.addon64
```

This deliberately differs from Feeder's normal upstream installation guide,
which includes `renodx-dlss5.addon64` because Feeder needs a neural provider.
For a Chicken test, Chicken substitutes for that component. Do not install both
providers: they would independently own or intercept the same NGX work. If both
are detected, Chicken stays inert, preserves the existing output, and tells you
to keep Feeder, remove the Reno neural provider, and fully restart.

Chicken deliberately treats `dlss5-feed.addon64` and
`dlss5-feed-host64.exe` as compatible transports, not competing neural
providers. Loading either RenoDX neural-provider filename at the same time
still latches Chicken off for the process. Game-specific RenoDX HDR add-ons
are unrelated and may remain for an initial test.

Set `warmup_rebuild=0` in `dlss5-feed.cfg`. Feeder's default warm-up rebuild
exists for older, single-scan neural-provider hooks. Chicken continuously
adopts later feature-1 creates, so the extra rebuild is unnecessary and can
look like a leak when a game is already changing resolution.

For ABI-aware builds, Chicken enforces the advertised one-Evaluate-per-Present
cadence for its own neural work while still calling the genuine Feeder target.
On a resolution, history, warm-up, or host recreation, Chicken fences and
releases only its feature-18 graph/codecs/intermediates. It never releases the
Feeder feature-1 handle, and recreation uses bounded slots rather than
allocating a new permanent graph every frame.

## 64-bit layout

Follow Feeder's official install for the game's API and motion-vector provider.
Place the three Chicken files and your trusted NVIDIA DLSSNR runtime beside the
real game executable and Feeder add-on:

```text
<game folder>\
  deep-fried-chicken.addon64
  deep-fried-chicken-nvngx.dll
  deep-fried-chicken.cfg
  dlss5-feed.addon64
  dlss5-feed.cfg
  nvngx_dlssnr.dll               supplied by the user, not by Chicken
  reshade-shaders\Shaders\DLSS5_Feed.fx
```

Do not add `dlss5-dx11-bridge.addon64`. That bridge is for a D3D11 game which
already owns native DLSS; Feeder is for constructing a synthetic contract in a
game which does not. Running both against the same frame creates two owners.

Chicken does not require `LoadFromDllMain` for the normal 64-bit Feeder path,
because Feeder creates its first NGX feature after ReShade effects initialize.
Early load remains harmless and is recommended when the game also loads NGX
during startup.

## 32-bit host layout

Use a complete, version-matched Feeder 32-bit package. Its game-side addon and
shader stay beside the 32-bit executable. Chicken and the 64-bit NVIDIA files
belong in Feeder's `host64` directory, because a 32-bit process cannot load
them:

```text
<game folder>\
  dlss5-feed.addon32
  dlss5-feed.cfg
  reshade-shaders\Shaders\DLSS5_Feed.fx
  host64\
    dlss5-feed-host64.exe
    dxgi.dll                       64-bit ReShade with add-on support
    deep-fried-chicken.addon64
    deep-fried-chicken-nvngx.dll
    deep-fried-chicken.cfg
    nvngx_dlss.dll
    nvngx_dlssnr.dll
```

Remove `host64\renodx-dlss5.addon64` and
`host64\renodx-dlss.addon64`. If the Chicken tab is absent from the helper's
ReShade overlay, add `deep-fried-chicken.addon64` to the helper's
`host64\ReShade.ini` `LoadFromDllMain` list and restart both processes.

Feeder v0.8 uses pipe protocol v3. Its `addon32`, host executable, layer files,
and helper configuration must all come from the same v0.8 package. Do not mix
v0.7 and v0.8 halves.

## Transport matrix

| Game path | Feeder responsibility | Chicken responsibility | Status |
| --- | --- | --- | --- |
| 64-bit D3D12 | Same-device synthetic DLAA contract; zero-copy guides | Output-to-output neural passes | Source-contract compatible; runtime community validation required |
| 64-bit D3D11 | Private D3D12 transport and optional 50-100% work extent | Neural passes at Feeder's work extent | Source-contract compatible; do not add the native-DLSS bridge |
| 64-bit Vulkan | Vulkan/D3D12 shared images and fences | Neural passes on Feeder's D3D12 endpoint | Source-contract compatible; follow Feeder's layer fallback |
| 64-bit OpenGL | OpenGL/D3D12 interop | Neural passes on Feeder's D3D12 endpoint | Source-contract compatible; upstream 64-bit game coverage is limited |
| 32-bit D3D11/OpenGL | x86 add-on plus x64 host transport | x64 addon inside `host64` | Source-contract compatible; full package must be version matched |
| 32-bit Vulkan/DXVK | x86 Vulkan layer plus x64 host | x64 addon inside `host64` | Experimental upstream beta; not claimed as game-validated here |
| D3D9 | dgVoodoo2 to D3D11, then the matching Feeder path | Same as translated D3D11 | Translation-dependent; follow upstream D3D9 instructions |

Feeder always publishes a 1:1 DLAA contract to NGX. Its D3D11 **Work
resolution** option reduces that whole contract to 50-100% and linearly scales
the result back to the native backbuffer. Chicken therefore runs at the chosen
work extent; this is not the same thing as a game's genuine DLSS Performance,
Balanced, or Quality contract.

Feeder v0.8 supplies color, raw depth, motion vectors, and a bias-current-color
trust mask on its 64-bit paths. Chicken preserves the depth and motion-vector
contract and forwards the trust mask only when the created feature has a
complete supported ABI-1 marker or qualifies for the exact legacy fallback,
and the mask resource covers the active guide extent. Upstream's
32-bit host does not currently publish that mask, so Chicken cannot invent it.

NVIDIA Smooth Motion and OptiScaler must be disabled for a Feeder test, as
required by Feeder upstream. Start with one Chicken pass and 100% Feeder work
resolution, Pass 1 at the golden Default preset/style and strength settings,
and Texture Boost off; then change one variable at a time. Pass 1 uses the same
NR controls in standard one-pass mode as it does in a longer chain. The shared
paper-white/HDR-transfer/color section wraps the complete chain and does not
change Feeder's API transport. Material Lab and Environment Detail are not
active in 1.4.0. HDR codec selection also checks the actual output carrier:
only a flagged floating-point carrier uses the linear-HDR path, while
UNORM/PQ-compatible carriers stay on the non-linear-safe path.

Chicken's smart host resolver validates executable Create/Evaluate/Release
ownership, deduplicates forwarding aliases, and keys every Feeder feature by
both hook slot and handle. It continues bounded discovery for a late-loaded
x64 host and never replaces the genuine address returned to Feeder by
`GetProcAddress`.

## Logs for a compatibility report

Send all of the following from the same run:

- `deep-fried-chicken.log`;
- `dlss5-feed.log`;
- `ReShade.log` from the game, plus `host64\ReShade.log` for 32-bit;
- game executable, bitness, API, output/work resolution, GPU, driver, and
  exact Feeder version;
- the Feeder `MV probe` and `Depth probe` lines after at least 600 frames.

A good Chicken log identifies `dlss5-feed.addon64` or
`dlss5-feed-host64.exe`, then reports a synthetic 1:1 game and neural contract.
Source-contract checks are not a substitute for a real game run, so new API or
bitness rows remain compatibility-test targets until community logs prove them.
