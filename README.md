> # 🔄 This project is being partially superseded
>
> ShortFuse's **renodx-dlss** add-on now handles **D3D9, D3D11, and D3D12 presentation natively**
> — in-process, through a same-adapter D3D12 endpoint, with real motion-vector and depth sharing
> on D3D11. **For any 64-bit D3D9 / D3D11 / D3D12 game, use that add-on directly — you do not
> need DLSS5-Feeder.**
>
> **Where to get it:** this build is *not* published on the
> [renodx GitHub](https://github.com/clshortfuse/renodx). It is distributed through the **RenoDX
> Discord, `#DLSS5` channel** — check the pinned messages there for the current binary:
> <https://discord.com/channels/1408098019194310818/1542647972695904317>
>
> DLSS5-Feeder remains the only option for what renodx-dlss does not cover:
>
> - **32-bit games** — renodx-dlss is 64-bit only, and NVIDIA ships no 32-bit NGX runtime, so an
>   in-process approach is impossible there. The feeder's cross-process host is the only way.
> - **Vulkan games** — via the bundled layer.
> - **Real motion vectors on D3D9** — renodx-dlss evaluates only the finished backbuffer there
>   (no temporal inputs); the feeder drives a full temporal evaluate from ReShade motion vectors.

> ## ⚠️ Pin the DLSS 5 neural-rendering add-on to v4.55
>
> `renodx-dlss5.addon64` — the separate add-on this project detours, required by every install
> below — has started building part of the synthetic DLSS contract itself in builds past v4.55.
> Running a newer build alongside DLSS5-Feeder conflicts. **Use v4.55**, from the same RenoDX
> Discord, `#DLSS5` channel:
> <https://discord.com/channels/1408098019194310818/1542647972695904317/1543568908017995818>
>
> **v4.6 status (2026-08-31):** building from current `main` adds v4.6 support. The feeder
> detects a v4.6 build (`NRToggleKey` marker), keeps the lazy-adoption path, writes
> `EnableHooks=2`, `NeuralUplift=1` and `NREnableUpscaling=0` when unset (v4.6 pairs its WIP
> upscaling with a rejection latch, and this feeder's contract is always 1:1 DLAA), and in the
> `host64` helper unbinds v4.6's new global hotkeys so a gameplay keypress cannot silently
> toggle NR in the background process. Older add-on builds are unaffected — every guard is
> keyed to a marker only newer builds carry, or writes a key only they read. Static analysis
> shows no remaining conflict, but **no game row has verified a v4.6 run yet** — with the
> released feeder builds, stay on v4.55.

> ## ⚠️ NVIDIA Smooth Motion and Optiscaler
>
> **Optiscaler** replaces the same upscaling path this feeder drives. Do not run both.
>
> **Smooth Motion** is not a pure driver feature: the driver injects `NvPresent64.dll` into the
> game, which hooks `CreateDXGIFactory*`, wraps the game's swapchain, and **calls `Present` more
> than once per game frame from its own pacer thread**. ReShade's effect chain — and so this
> add-on — runs *inside* `Present`, so that turns one caller per frame into several, possibly
> concurrent ones. Two open reports: corruption or a silent stop in D3D11
> ([#1](https://github.com/jlrouzies-fr/DLSS5-Feeder/issues/1)), flicker in Vulkan
> ([#10](https://github.com/jlrouzies-fr/DLSS5-Feeder/issues/10)).
>
> **Since 0.8.0 the feeder serializes its whole per-frame path and turns on D3D11 multithread
> protection**, and it detects Smooth Motion and says so in the overlay and `dlss5-feed.log`. That
> removes the feeder-side race. First data point (2026-09-01, 0.8.0-beta.4): Metro 2033 Redux ran
> with Smooth Motion **active** — session open, frames delivered at the usual per-frame cost, no
> re-entrant or off-thread Present observed. Full visual verification is still pending, and the
> Vulkan path has no Smooth Motion run yet.
>
> Two things Smooth Motion *did* break on that machine, neither of them this feeder — both crash
> the game 1–2 s into boot with a null read on the present path, before the feeder feeds a frame:
>
> - **Luma** (`Luma-Metro Redux.addon`): reads `GetCurrentBackBufferIndex()` and passes it to
>   `GetBuffer()`, which fails under Smooth Motion's flip-model wrapper; the null back buffer is
>   then dereferenced. Fix offered upstream to Filoppi/Luma-Framework. Remove the Luma add-on to
>   boot, or turn Smooth Motion off for the game.
> - **`NRStyle=2`** (RenoDX v4.6's own setting, from its overlay panel): crashes at the next boot
>   even without Luma. The feeder now warns when it sees it; set `NRStyle=0` in `ReShade.ini`'s
>   `[RenoDX.DLSS5]` section to recover.
>
> If the image corrupts or flickers, turn Smooth Motion off **for this game's API only** rather
> than everywhere, in NVIDIA Profile Inspector:
>
> | Setting | ID | Value |
> | --- | --- | --- |
> | Smooth Motion - Enabled APIs | `0xB0CC0875` | bitfield, default `7`: `1` DX12, `2` DX11, `4` Vulkan — clear only the bit for this game |
> | Smooth Motion - Enable | `0xB0D384C0` | `0` / `1`, per application |
> | Smooth Motion - Debug Bars | `0xB01B8B02` | draws coloured bars on generated frames — use it to check whether bad frames *are* the generated ones |

# DLSS5-Feeder

**DLSS 5 neural rendering in games that ship without any DLSS — D3D11, D3D12, Vulkan, OpenGL, 32-bit, even DirectX 9.**

DLSS 5's neural-rendering add-on only works by hooking a game's own DLSS calls. A game that has no
DLSS never makes those calls, so the add-on sits idle. **DLSS5-Feeder makes the calls itself.** It
builds a complete DLSS DLAA "contract" out of what ReShade already has — the frame being processed,
the depth buffer, and estimated optical-flow motion vectors — runs a genuine DLSS evaluate, lets the
DLSS 5 neural-rendering add-on hook into that evaluate, and copies the neural result back into the
frame. All inside ReShade's effect chain.

```
game frame → ReShade effects → [motion vectors] → [DLSS5_Feed] → DLSS5-Feeder:
                                                    depth + MV     DLSS DLAA + DLSS 5 neural rendering
                                                                   ↓
                                    neural output written back over the frame → later effects → present
```

## Contents

- [Status](#status)
- [Install for a 64-bit game](#install-for-a-64-bit-game)
- [Install for a 32-bit game](#install-for-a-32-bit-game-beta)
- [Install for a DirectX 9 game](#install-for-a-directx-9-game-beta)
- [Install for a Vulkan game](#install-for-a-vulkan-game)
  - [32-bit Vulkan (DXVK)](#32-bit-vulkan-dxvk)
- [Install for an OpenGL game](#install-for-an-opengl-game)
- [Motion vectors: choosing a provider](#motion-vectors-choosing-a-provider)
- [How it works](#how-it-works)
  - [The 32-bit path](#the-32-bit-path)
  - [The DirectX 9 path](#the-directx-9-path)
  - [The Vulkan path](#the-vulkan-path)
  - [The OpenGL path](#the-opengl-path)
- [Requirements](#requirements)
- [Configuration](#configuration)
- [Logs and troubleshooting](#logs-and-troubleshooting)
- [Building](#building)
- [Limitations and roadmap](#limitations-and-roadmap)
- [Credits](#credits)
- [License](#license)

## Status

Proven working in seven games covering every supported path:

| Game | Bitness / API | Result |
| --- | --- | --- |
| **Metro 2033 Redux** | 64-bit D3D11 | 4K DLAA + NR, HDR backbuffer |
| **Subnautica** | 64-bit D3D11 | Contributor-verified: 4K DLAA + NR, Generic Depth clear-selection profile |
| **The Lord of the Rings: War in the North – Legacy Edition** | 64-bit D3D12 | 4K, same-device path, 120 fps |
| **Splinter Cell: Blacklist** | 32-bit D3D11 | 60 fps, cross-process host |
| **BioShock Remastered** | 32-bit D3D11 (D3D9→D3D11 wrapper) | 4K, Luma HDR |
| **Fable Anniversary** | 32-bit **D3D9** via dgVoodoo2 | 1440p, 60 fps |
| **DOOM (2016)** | 64-bit **Vulkan** | 4K, D3D12 evaluate via cross-API interop |
| **Worms Ultimate Mayhem** | 32-bit **OpenGL** | 4K, GL↔D3D12 interop + cross-process host, 0.13 ms/frame |

In each, the DLSS 5 add-on reports `feature 18 created … inline feature 18 evaluation succeeded`,
driven entirely by ReShade depth + estimated motion vectors.

It is not game-specific: any D3D11, D3D12, Vulkan or OpenGL game with a working ReShade depth buffer
and a motion vector provider should work — 64-bit directly, 32-bit via a bundled 64-bit helper
process, D3D9 via a wrapper.

**32-bit Vulkan (DXVK) is implemented but has no game row yet** (issue #15). The transport is the
same `src/feed_vk.h` the 64-bit Vulkan path uses, compiled x86, with the host creating the shared
textures the way the OpenGL path already does; the cross-bitness half is proven end-to-end on this
hardware by `spike\spike-vkhost64.exe` + `spike\spike-vkclient32.exe`. Treat it as untested in a
real game until a row appears above. See [`PLAN-VULKAN32.md`](PLAN-VULKAN32.md).

**The OpenGL path is verified 32-bit-first**, which is the harder of its two halves: Worms Ultimate
Mayhem runs the full cross-process route — the host creates the shared textures (GL memory objects
are import-only), the game imports them raw and answers on a shared D3D12 fence. The 64-bit
in-process OpenGL path shares that same `src/feed_gl.h` transport and is proven by
`spike\spike-gl64.exe`, but has no game row of its own yet. See
[The OpenGL path](#the-opengl-path) and [`PLAN-OPENGL.md`](PLAN-OPENGL.md).

**This is beta software.** Expect the temporal quality of *estimated* motion vectors (some ghosting
in fast motion, softness on thin moving geometry), and the HUD is processed along with the scene.

> ### 0.6.1: read this before installing
>
> **The motion-vector provider is now chosen with a preprocessor definition** (`DLSS5_MV_PROVIDER`)
> instead of being whichever shader happens to write `texMotionVectors`. Five providers are
> supported and the recommended one is **[LumeniteFX](https://github.com/umar-afzaal/LumeniteFX)
> Kernel** — see [Motion vectors: choosing a provider](#motion-vectors-choosing-a-provider).
>
> **If you installed a release up to 0.5.2 and followed the old instructions, your feed has most
> likely been running on zero motion vectors.** ReshadeMotionEstimation (DRME), which those releases
> recommended, **does not compile on ReShade 6.8** (`error X3020: cannot sample from texture that is
> also used as render target`). ReShade still lists it as an enabled technique, so nothing looked
> wrong — but it wrote nothing. This release detects that and says so in the overlay and the log.

## Install for a 64-bit game

1. Run **ReShade's installer** (https://reshade.me), point it at your game's `.exe`, choose
   **Direct3D 10/11/12**, and tick **"Enable loading of add-ons"**.
2. Download **`dlss5-feed.addon64`** and **`DLSS5_Feed.fx`** from the
   **[latest release](https://github.com/jlrouzies-fr/DLSS5-Feeder/releases/latest)**. Put
   `dlss5-feed.addon64` next to the game `.exe`, and `DLSS5_Feed.fx` into `reshade-shaders\Shaders\`.
   The shader includes the standard **`ReShade.fxh`** header. ReShade normally installs it with its
   standard shader package; if `ReShade.log` says it cannot open that file, copy `ReShade.fxh` from
   the official [reshade-shaders](https://github.com/crosire/reshade-shaders/tree/slim/Shaders)
   repository into the same `Shaders\` folder.
3. Download **[LumeniteFX](https://github.com/umar-afzaal/LumeniteFX)** (Code ▸ Download ZIP). Copy
   its `Shaders\` folder (the `lumenite_*.fx` files and `include\`) into `reshade-shaders\Shaders\`,
   and `Textures\lumenite_bluenoise256.png` into `reshade-shaders\Textures\`.
   *(Other providers: see [Motion vectors: choosing a provider](#motion-vectors-choosing-a-provider).)*
4. Get **`renodx-dlss5.addon64`** (**v4.55** — see the warning above) and **`nvngx_dlssnr.dll`** from
   the RenoDX Discord. Put both next to the game `.exe`, plus a **`nvngx_dlss.dll`** (from any DLSS
   game, or [DLSS Swapper](https://github.com/beeradmoore/dlss-swapper)).
5. Press **Home** for the ReShade overlay, select `DLSS5_Feed.fx`, set **`DLSS5_MV_PROVIDER` = `3`**
   in its Preprocessor definitions, and reload effects.
6. In-game: enable **"LUMENITE: Kernel 2.0"**, then **DLSS 5 Feed** below it, then turn on neural
   rendering in the **DLSS 5 Neural Rendering** panel. Turn the game's MSAA/SSAA **off**.

Check `dlss5-feed.log` (next to the game `.exe`) for `feature ready … DLAA`, `frame N delivered`,
and `DLSS5_MV_PROVIDER=3 (LumeniteFX Kernel) -> Lumenite_Kernel (enabled)`. The overlay's
**Motion vectors** section shows the same, in red if the shader and the enabled provider disagree.
`dlss5-feed.cfg` is created automatically with working defaults.

> **Do I need the DLSS 5 DX11 *bridge*?** **No.** DLSS5-Feeder does the bridge's job for games that
> have no DLSS. The bridge — **https://github.com/NIGos/dlss5-dx11-bridge/releases** — is only for
> DX11 games that *already* have their own DLSS; don't run both for the same game.

## Install for a 32-bit game (beta)

32-bit games need one extra piece: a 64-bit helper that does the actual DLSS work, since NGX only
exists as 64-bit code.

1. Run ReShade's installer, point it at your game's `.exe` — it detects **32-bit** automatically.
2. Download **`dlss5-feed.addon32`**, **`DLSS5_Feed.fx`** and **`dlss5-feed-host64.exe`** from the
   **[latest release](https://github.com/jlrouzies-fr/DLSS5-Feeder/releases/latest)**. Put
   `dlss5-feed.addon32` next to the game `.exe`, `DLSS5_Feed.fx` into `reshade-shaders\Shaders\`,
   and `dlss5-feed-host64.exe` into a new `host64\` folder next to the game `.exe`.
3. Put a 64-bit ReShade `dxgi.dll`, `renodx-dlss5.addon64` (**v4.55** — see the warning above),
   `nvngx_dlssnr.dll` and `nvngx_dlss.dll` into `host64\`. (Get the x64 `dxgi.dll` by running the
   ReShade installer once against any 64-bit game.)
4. Install a motion-vector provider into the game's `reshade-shaders\`, same as steps 3 and 5 of the
   [64-bit instructions](#install-for-a-64-bit-game).
5. Turn it on in-game as above. Day-to-day DLSS 5 settings live in **ReShade's overlay → Add-ons tab
   → DLSS 5 Feed** — see [Configuration](#configuration).

The first fed frame also spawns `host64\dlss5-feed-host64.exe`, which opens a window titled
**"32-bit DLSS 5 Feeder"** — the add-on and the game never share a ReShade instance, so this is
where the DLSS 5 add-on's *own* full panel lives, for anything not covered by our overlay page.
Press Home in that window to open it:

<img width="1880" height="1058" alt="image" src="https://github.com/user-attachments/assets/57abd732-94d2-401c-a524-6536006f3c86" />

## Install for a DirectX 9 game (beta)

D3D9 games need a translation layer first — **[dgVoodoo2](http://dege.freeweb.hu/dgVoodoo2/)** turns
D3D9 into D3D11, and everything after that is a normal 32-bit install.

**Not sure if you need this?** Launch the game with ReShade installed and check `ReShade.log`:
`IDirect3DDevice9` means yes; `D3D11CreateDevice` means the game already runs on D3D11 — skip to
[Install for a 32-bit game](#install-for-a-32-bit-game-beta).

1. Download dgVoodoo2 from **http://dege.freeweb.hu/dgVoodoo2/** and unzip it.
2. Copy `MS\x86\D3D9.dll`, `dgVoodoo.conf` and `dgVoodooCpl.exe` next to the game's `.exe` (often
   not the game's root folder — Fable Anniversary's is `Binaries\Win32\`).
3. Run `dgVoodooCpl.exe` from that folder (or edit `dgVoodoo.conf` directly). In `[DirectX]`:

   | Setting | Value | Why |
   | --- | --- | --- |
   | `DisableAndPassThru` | **`false`** | Ships as `true`, which disables dgVoodoo entirely — the #1 reason "dgVoodoo doesn't seem to do anything". |
   | `VRAM` | **`1GB`** | The default (256 MB) causes "ran out of video memory" crashes regardless of your real GPU. Don't use `2GB` — some old engines mishandle it. |
   | `VideoCard` | `internal3D` | dgVoodoo's own virtual card; the most capable option. |
   | `dgVoodooWatermark` | `true` | Temporary — confirms dgVoodoo is actually running. |

   In `[General]`: `OutputAPI = d3d11_fl11_0` (or higher).
4. Launch the game — the **dgVoodoo watermark must appear**, or nothing else will work.
5. Follow [Install for a 32-bit game](#install-for-a-32-bit-game-beta) (or the 64-bit steps for a
   64-bit D3D9 game). Install ReShade as `dxgi.dll`, never `d3d9.dll` — dgVoodoo owns that name.
6. Turn the watermark off once everything works.

## Install for a Vulkan game

Same pieces as a 64-bit game — with two differences.

1. Run ReShade's installer, point it at your game's `.exe`, and choose **Vulkan**.
2. Add `AddonPath=.\` under `[ADDON]` in the game's `ReShade.ini` (next to the exe).
3. Everything else is identical to the [64-bit instructions](#install-for-a-64-bit-game).

Most Vulkan games don't enable the extensions this needs — **the add-on adds them automatically**,
so there's nothing else to configure. See [The Vulkan path](#the-vulkan-path) for the mechanism.

**If `dlss5-feed.log` says the interop entry points are missing**, launch through the bundled
fallback layer instead:

```
layer\run-with-feed-layer.bat "E:\path\to\game.exe"
```

See [`layer/README.md`](layer/README.md) — it does the same job from outside the process.

### 32-bit Vulkan (DXVK)

Almost every 32-bit game that reaches Vulkan does it through
**[DXVK](https://github.com/doitsujin/dxvk)**. Two differences:

1. Install ReShade **as a Vulkan layer**, not as a local `dxgi.dll` or `d3d9.dll` — DXVK owns those
   names in the game folder.
2. Add the `host64\` folder from [Install for a 32-bit game](#install-for-a-32-bit-game-beta), and
   install both halves from the same release.

DLSS runs at the game's native resolution here, so the **Work resolution** slider is fixed at 100%.

If `dlss5-feed.log` says the interop entry points are missing, use the 32-bit fallback layer:

```
layer\x86\run-with-feed-layer32.bat "E:\path\to\game.exe"
```

## Install for an OpenGL game

The simplest of the four — nothing extra to configure.

1. Run ReShade's installer, point it at your game's `.exe`, and choose **OpenGL**.
2. Everything else is identical to the [64-bit instructions](#install-for-a-64-bit-game).

For a **32-bit** OpenGL game, follow [Install for a 32-bit game](#install-for-a-32-bit-game-beta) —
install both halves from the same release.

> **Hybrid laptops:** force the game onto the NVIDIA GPU (Windows **Settings ▸ Display ▸ Graphics**).
> Otherwise the feed disables itself — DLSS needs that GPU.

## Motion vectors: choosing a provider

DLSS5-Feeder does not estimate motion itself — it reads the output of a motion-vector shader you
install. Which one it reads is fixed **at compile time** by the `DLSS5_MV_PROVIDER` preprocessor
definition on `DLSS5_Feed.fx` (ReShade overlay → select `DLSS5_Feed.fx` → *Preprocessor
definitions* → reload effects):

| Value | Provider | Enable this technique | Notes |
| --- | --- | --- | --- |
| `0` *(default)* | Anything writing the shared **`texMotionVectors`** — qUINT, `dh_uber_motion`, DRME | that shader's own | The old convention. **DRME does not compile on ReShade 6.8** (see below). |
| `1` | **iMMERSE Launchpad** (MartysMods) | `Launchpad` | Also files Launchpad's per-frame optical-flow request, so it works without iMMERSE RTGI running. Warping around flames/transparents is worst here. |
| `2` | **VORT** | `vort_Motion` | MIT. |
| **`3`** | **LumeniteFX Kernel** ← **recommended** | `LUMENITE: Kernel 2.0` | 1/8-resolution flow **plus a confidence map**, which the feed uses. The configuration this beta was tuned on. |
| `4` | **LumeniteFX QuantMotion** | `LUMENITE: QuantMotion` | Same shape as Kernel, different estimator. |

Rules that apply to all of them:

* The provider's technique must be **enabled and above `DLSS 5 Feed`** in the technique list.
* Only **one** provider should be enabled. The add-on warns (overlay + log) when the shader is
  compiled for one provider while a different one is enabled — the classic silent failure.
* Nothing of any provider is bundled or `#include`d here: the shader declares the provider's output
  texture **identically** to the provider itself, so ReShade binds the same resource. Only the
  selected provider's texture is allocated.

> **ReshadeMotionEstimation (DRME) does not compile on ReShade 6.8**
> (`error X3020: 'V__texCur0': cannot sample from texture that is also used as render target`).
> It still appears as a technique and can be "enabled", but it writes nothing, so DLSS runs with no
> motion vectors at all. Releases up to 0.5.2 recommended it. This release reads the compiler error
> out of `ReShade.log` and reports it in the overlay and `dlss5-feed.log`.

### Are the guides actually arriving?

The add-on checks both configuration and the actual textures handed to DLSS:

* The overlay's **Motion vectors** section states the mode, the provider found, and its state
  (`enabled` / `DISABLED` / `FAILED TO COMPILE` / `not installed`), in red when something is wrong.
* A low-frequency **guide probe** reads back the vectors *actually handed to DLSS* every 600 frames
  and logs their mean/max magnitude and non-zero share: `MV probe … N% non-zero`. While you move,
  that must not be 0%. The same deferred readback samples four distributed depth blocks and reports
  min/max/mean/variance/finite share. It warns when sampled depth is flat without disabling the feed.
  Both readbacks analyse an older, completed copy, so they do not introduce a per-frame GPU stall.

### Validation and the trust mask

Every provider here is *optical flow*: it answers a lighting change — a flickering light, a flame —
with a confident vector pointing at whatever happened to match, and DLSS then warps history in from
there. `DLSS5_Feed.fx` now reprojects each vector into the previous frame and checks it: depth
(disocclusion), vector consistency, and a *static-hypothesis* test asking whether "did not move"
explains the pixel better, on illumination-normalised structure. Vectors that fail are zeroed, and
the pixel is flagged in a `DLSS5_Mask` texture the add-on passes to DLSS as its
**bias-current-colour mask** — DLSS's own mechanism for "don't trust history for this pixel"
(all three transports; the 32-bit add-on does not pass it yet). The defaults are the tuned ones.

## How it works

* `DLSS5_Feed.fx` (companion effect) converts the selected provider's motion vectors (delta-UV,
  `prev_uv = uv + mv`) into `DLSS5_MV` (RG16F, **pixels**), copies the raw hardware depth with
  ReShade's orientation fixes into `DLSS5_Depth` (R32F), and flags every vector it does not trust
  in `DLSS5_Mask` (R8). MV, mask and raw depth are emitted by one MRT guide pass, avoiding a second
  full-screen depth pass while leaving the depth values sent to DLSS unchanged.
* `dlss5-feed.addon64` registers with the ReShade add-on API. After the `DLSS5_Feed` technique
  renders, it takes the backbuffer + those textures and runs `NGX_D3D12_EVALUATE_DLSS` in DLAA
  mode (render size = output size, no jitter). The DLSS 5 neural-rendering add-on
  (`renodx-dlss5.addon64`) detours that D3D12 evaluate and inserts its neural pass — it cannot tell
  the contract is synthetic.
* On a **64-bit D3D11 game** the optional **Work resolution** slider can run the private
  DLAA + Neural Rendering contract at 50–100% of each backbuffer axis. Color is resampled
  linearly; depth, motion vectors and the trust mask use point sampling; motion-vector
  magnitudes are corrected for the selected work extent. The result is linearly expanded
  back over the native-size backbuffer. At 100% this reduces to the original copy path.
* On a **D3D12 game** there is no transport at all: NGX runs on the game's own device and queue,
  motion vectors and depth are consumed zero-copy straight from the effect textures, and the feature
  survives alt-tabs and effect reloads untouched (only a real resolution change rebuilds).
* NGX calls are wrapped in SEH, a command list the add-on crashed in is discarded rather than
  submitted, and NGX is reinitialized after repeated failures — a faulting closed-source add-on
  disables the feed instead of taking the game down.

### The 32-bit path

NGX and the DLSS 5 add-on only exist as x64 code, and a 32-bit process cannot load an x64 DLL — so
`dlss5-feed.addon32` does none of the NGX work itself. Instead:

* It creates the four Color/Output/Depth/MV textures as **cross-process shared** D3D11 resources
  (`D3D11_RESOURCE_MISC_SHARED_NTHANDLE`) on the game's own device, plus two shared fences.
* It spawns `dlss5-feed-host64.exe` and hands it the texture/fence handles over a named pipe
  (`DuplicateHandle` across the process boundary — the same WDDM sharing the driver already uses,
  just one hop further).
* The host — a genuine 64-bit process — opens those shared resources on **its own D3D12 device**,
  runs the same DLSS DLAA evaluate the 64-bit add-on runs in-process, and signals a fence back.
  No frame data ever crosses into system memory; every copy stays GPU-to-GPU.
* Because the DLSS 5 add-on is itself a ReShade add-on, the host disguises itself as a game to load
  it: a window with a minimal D3D12 swap chain lets its own bundled ReShade (`host64\dxgi.dll`)
  attach and the add-on arm its hooks, exactly as in a real D3D12 title. The 32-bit `dlss5-feed.cfg`
  add-on writes settings changes made in the *game's* own overlay straight into that window's
  ReShade.ini and restarts it to apply them.
* If the host process dies, the pipe breaks, the add-on notices and disables itself — the game keeps
  rendering normally.
* Verified end to end with a deliberate split-screen test (`mode=1`): the host copies only the left
  half of the frame back, so a visibly half-black screen proves the full round trip — game → shared
  texture → host → shared fence → game's backbuffer — actually reaches the display, not just the logs.

### The DirectX 9 path

dgVoodoo2 sits in front as `D3D9.dll` and translates the game's D3D9 calls onto its own D3D11 device.
ReShade (installed as `dxgi.dll`) hooks that D3D11 device rather than the game's D3D9 one, so from
DLSS5-Feeder's point of view it is simply a D3D11 game and the 32-bit path applies unchanged. The
translation is what makes SM5 motion-vector shaders, shared NT-handle textures and fences available
at all — none of which exist on real D3D9.

### The Vulkan path

The DLSS 5 add-on only hooks **D3D12** NGX entry points, so even though NGX has a Vulkan API, using
it would be pointless — the add-on would never see the call. The evaluate therefore runs on a
private D3D12 device exactly as on the D3D11 path, and the frame crosses the API boundary through
shared memory rather than being copied out to system RAM:

* The D3D12 side creates the shared textures and two shared fences (`D3D12_HEAP_FLAG_SHARED`,
  `D3D12_FENCE_FLAG_SHARED`) and exports NT handles for them.
* The add-on imports those handles into the game's own `VkDevice` with raw Vulkan — the D3D12
  external types (`VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT`,
  `VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT`, dedicated allocation). A D3D12 fence and a
  Vulkan timeline semaphore are the same object, so the frame counter crosses unchanged.
* The resulting `VkSemaphore`s are handed back to ReShade as `api::fence` handles (in ReShade's
  Vulkan backend they *are* those objects), so per-frame queue signal/wait stay inside ReShade's own
  locks. A raw `vkQueueSubmit` would race the game's and ReShade's submits.
* Per frame: ReShade's `barrier()` moves the game's own images (its layout tracking stays correct),
  raw `vkCmdCopyImage`/`vkCmdBlitImage` move pixels into and out of our imported images, which sit
  permanently in `VK_IMAGE_LAYOUT_GENERAL`.

The interop extensions are fixed at `vkCreateDevice`, and games rarely enable them (ReShade adds
`external_memory_win32` and `timeline_semaphore` itself, but not the semaphore/dedicated-allocation
ones). The add-on is already in the process by then — ReShade's Vulkan layer loads add-ons inside
its `vkCreateInstance` hook and fires `create_device` from there — so on that event it puts an
inline hook (MinHook) on `vulkan-1.dll`'s exported `vkCreateDevice` (`src/feed_vk_hook.h`). The
loader returns that same export for `vkGetInstanceProcAddr(instance, "vkCreateDevice")`, so every
loading style lands in the hook, above ReShade's own layer, which then passes the extended list
down. The hook is removed on DLL unload, since ReShade reloads add-ons per Vulkan instance.
[`layer/VkLayer_feed_vk.dll`](layer/README.md) does the same from outside the process, as a fallback.

**32-bit Vulkan** (DXVK) reuses every one of those pieces — `src/feed_vk.h` and
`src/feed_vk_hook.h` are compiled into the x86 add-on unchanged — with the D3D12 middle moved out
to the helper process, exactly as on the 32-bit D3D11 and OpenGL paths. One thing flips: the
**host** creates the shared textures and duplicates the handles into the game, because D3D12 cannot
open memory Vulkan exported. That is the same direction the OpenGL path already uses, so the pipe
protocol needed only a new client kind and one extra field — the host owns the Output *format*
there, since only its device can be asked whether this GPU supports a typed UAV store to BGRA8, and
DXVK swapchains are almost always BGRA8. Getting that wrong is issue #11's washed-out image again.
`spike\spike-vkhost64.exe` + `spike\spike-vkclient32.exe` prove the cross-bitness half on its own.

### The OpenGL path

Same shape as the Vulkan path — a private D3D12 device creates the shared textures and fences, and
the game's API imports them — but everything the Vulkan path had to fight for comes free here, and
one thing it could rely on does not exist.

**What comes free.** There is no device hook and no layer. Vulkan bakes extensions in at
`vkCreateDevice`, which is why the add-on has to hook that call; OpenGL has no equivalent opt-in —
extensions are a property of the driver's context, resolved at runtime through `wglGetProcAddress`.
If `GL_EXT_memory_object_win32` and `GL_EXT_semaphore_win32` are in the extension string, the
transport works. If they are not, the frame is not being rendered on an NVIDIA GPU, so DLSS could
not run either way and the feed says so and stops. Add-on discovery is also a non-question: ReShade
*is* the local `opengl32.dll`, and add-ons load from its own directory.

**What does not exist.** On Vulkan, ReShade's `api::fence` *is* a `VkSemaphore`, so the imported
objects could be handed back and every queue operation kept inside ReShade's locks. On OpenGL an
`api::fence` is documented as "an opaque value" — there is no way to wrap a raw GL semaphore into
one. So the whole per-frame GL side is raw ([`src/feed_gl.h`](src/feed_gl.h)), which is safe here
precisely where it was not on Vulkan: **OpenGL has no queue object.** Every command enters the
current context's single in-order stream on the calling thread, and `reshade_render_technique` fires
while ReShade is itself issuing GL commands on that thread and context. Our calls interleave in
program order — there is no lock to bypass, and no barriers are needed at all.

Per frame, inside the technique callback:

* `glCopyImageSubData` copies the motion vectors, depth and trust mask into our imported aliases
  (exact formats, no state touched); the colour is captured with `glBlitFramebuffer`, which converts
  formats and channel order and can read what a raw copy cannot — a renderbuffer or the default
  framebuffer.
* `glSemaphoreParameterui64vEXT(GL_D3D12_FENCE_VALUE_EXT)` + `glSignalSemaphoreEXT` + `glFlush`
  hands the frame to D3D12 (a D3D12 fence *is* a GL "D3D12 fence" semaphore, so the counter crosses
  unchanged). The flush matters: without it the signal can sit in the client command buffer while
  D3D12's GPU-side wait starves.
* D3D12 waits, evaluates, and signals back — or CPU-signals on any failure, because
  `glWaitSemaphoreEXT` has **no timeout** and a missing signal would hang the GL stream.
* `glWaitSemaphoreEXT` stalls the GL stream on the GPU (never the CPU), and a final blit puts the
  output back over the technique's render target.

A small state guard saves and restores exactly what the blits touch — the two framebuffer bindings,
the read/draw buffer selection, scissor and `GL_FRAMEBUFFER_SRGB` — and nothing else. `GL_FRAMEBUFFER_SRGB`
is forced **off** for our blits on purpose: the frame we are handed is already encoded, and an sRGB
encode on the way home is the OpenGL flavour of the washed-out image of issue #11.

Because GL has no sized BGRA8 internal format and we choose the shared textures' formats, the GL
path folds `B8G8R8A8`/`B8G8R8X8` to `R8G8B8A8` — harmless, because a blit is component-wise rather
than byte-order-preserving. GL names live in the share group of the context current at import, so
every frame checks `wglGetCurrentContext()` and rebuilds the session if the game switched contexts.

The 32-bit OpenGL path uses the very same header, compiled x86, over the existing helper-process
protocol — with one change forced by the API: **the host creates the shared textures**, because GL
memory objects are import-only and a GL process cannot export one. Both directions
(x86 extension parity, cross-process D3D12→GL import) are proven by `spike\spike-gl32.exe`.

## Requirements

| Piece | Notes |
| --- | --- |
| D3D11, D3D12, Vulkan or OpenGL game, 32- or 64-bit | NGX is 64-bit only, hence the helper process for 32-bit games. D3D9 works through [dgVoodoo2](#install-for-a-directx-9-game-beta); Vulkan works out of the box at both bitnesses (the add-on adds the interop extensions itself; a small bundled layer is the fallback — [64-bit](#install-for-a-vulkan-game), [32-bit/DXVK](#32-bit-vulkan-dxvk)); OpenGL needs nothing extra at all, but the game must be rendering on the NVIDIA GPU (see [Install for an OpenGL game](#install-for-an-opengl-game)). D3D10 is not supported. |
| ReShade 6.8+ **with add-on support** | Generic Depth add-on enabled and picking the scene depth. |
| DLSS 5 neural-rendering add-on (`renodx-dlss5.addon64`) + `nvngx_dlssnr.dll` | from its own author, **pinned to v4.55** — newer builds conflict with this project (see the warning near the top). Not included here. |
| `nvngx_dlss.dll` | a DLSS Super Resolution runtime next to the game (the driver's copy is used otherwise). |
| A motion vector provider | one of five, selected with the `DLSS5_MV_PROVIDER` definition — **[LumeniteFX](https://github.com/umar-afzaal/LumeniteFX) Kernel is recommended** (`=3`); also iMMERSE Launchpad, VORT, LumeniteFX QuantMotion, or anything writing `texMotionVectors` (qUINT, `dh_uber_motion`). **Not DRME — it does not compile on ReShade 6.8.** See [Motion vectors: choosing a provider](#motion-vectors-choosing-a-provider). Install it yourself — nothing third-party is bundled, and our shader includes no third-party files. |
| `dlss5-feed.addon64` (or `.addon32` + `host64\`) + `DLSS5_Feed.fx` | this project. |

## Configuration

The easiest way to change any of this is **ReShade's overlay → Add-ons tab → DLSS 5 Feed**: every
setting below is a live control there (checkboxes, sliders, combos), reading from and saving straight
to `dlss5-feed.cfg`. On 32-bit games the same page also shows the **DLSS 5 host's** neural-rendering
settings (neural uplift, NR intensity/style/local structure/local tone/auto mask/UI correction) with
an **Apply** button — since those live in a separate process, Apply writes them into
`host64\ReShade.ini` and restarts the helper (~2 s without DLSS; the game keeps rendering, the feed
reconnects automatically).

`dlss5-feed.cfg` itself is created automatically next to the add-on and re-read while the game runs,
if you prefer editing the file directly:

| Key | Default | Meaning |
| --- | --- | --- |
| `enabled` | 1 | 0 disables everything. |
| `mode` | 2 | 0 inert · 1 transport test (no NGX; on 32-bit it copies only the left half, so a split screen proves the round trip) · 2 full DLSS path. |
| `work_resolution` | 100 | **64-bit D3D11 only.** 50–100% of each backbuffer axis for the private DLAA + Neural Rendering work textures. The Add-ons overlay slider applies once 400 ms after dragging stops. Other paths remain at 100%. |
| `hdr` | -1 | -1 auto (FP16 / R11G11B10 backbuffer = HDR), 0 force SDR, 1 force HDR. |
| `depth_inverted` | -1 | -1 follow `RESHADE_DEPTH_INPUT_IS_REVERSED`, 0/1 force. |
| `flags` | -1 | raw `DLSS.Feature.Create.Flags` override. |
| `reset_every` | 0 | 1 = NGX Reset every frame (no temporal history; diagnostic). |
| `warmup_rebuild` | 180 | re-create the feature once after N delivered frames (works around the DLSS 5 add-on latching STANDBY on its first create; skipped automatically on newer "v45+" add-on builds). |
| `rebuild` | 0 | change the number to re-create the feature once, by hand. |
| `log_frames` | 3 | first N frames logged in detail. |
| `create_delay` | 60 | frames to hold a feature (re)build after a runtime (re)init — the DLSS 5 add-on arms its NGX hooks asynchronously, and calling in too early can crash. 0 disables. |
| `preset` | 0 | DLSS render-preset hint: `0` default, `5`/`6` = legacy CNN presets E/F (clamp history harder — try these if motion warps around transparents like dust or flames), `10`/`11` = transformer presets J/K. |
| `gpu_timeout_ms` | 2000 | how long a frame waits for the GPU to retire a command allocator before that frame is abandoned. Three abandoned frames in a row stop the feed. Raise it on a heavily contended GPU; clamped to 100–60000. |
| `mv_scale_x/y` | 1.0 | extra motion-vector multiplier. |
| `host_window` | 1 | **32-bit games only.** 1 shows the helper's window; 0 hides it (its own settings are now on the overlay page above, so you rarely need it). |

In `DLSS5_Feed.fx`'s own UI (settings that only make sense per-shader, not per-session):

* **MV_SIGN** — if the image doubles or smears while moving, flip a component.
* **Validation** — *Validate motion vectors* and its four tests (static hypothesis, luma, depth,
  consistency) with their thresholds. **The defaults are the tuned ones**; the usual reason to
  touch them is diagnosis. Setting *Depth tolerance* to `0` does not mean "strict" — it rejects
  effectively every vector, which is stable but motionless.
* **Use geometry vectors** — **experimental, off, not recommended.** Fits a camera model from the
  provider's flow + depth and derives static pixels' vectors from geometry. It removes the
  flame/flicker warping by construction, but the fit is noisy frame to frame and the HUD, which is
  not part of the 3D world, gets camera vectors it should not have.
* **DLSS 5 Feed – debug view** technique — nine views: the vectors/depth being sent (static scene =
  grey, motion = colour), the provider's confidence map, the validation mask alone and over the
  image, which test fired, and the three geometry views. The depth view applies a display-only
  contrast curve so reversed and far-heavy hardware depth is visible; `DLSS5_Depth` itself stays raw.

Preprocessor definitions on the shader (overlay → *Preprocessor definitions* → reload effects):

| Definition | Default | Meaning |
| --- | --- | --- |
| `DLSS5_MV_PROVIDER` | `0` | Which provider's output to read — see [the provider table](#motion-vectors-choosing-a-provider). |

## Logs and troubleshooting

| File | Contents |
| --- | --- |
| `dlss5-feed.log` | next to the game exe: resolved effect handles, the session, the contract (`feature ready: WxH DLAA, flags=…`), `frame N delivered`, timing and guide probes every 600 frames, crash breadcrumbs. |
| `ReShade.log` | which graphics API ReShade attached to, shader compile errors. |
| `host64\dlss5-feed-host.log` | 32-bit games: the helper's own session and per-frame state. |
| `host64\ReShade.log` | 32-bit games: the DLSS 5 add-on's messages (`feature 18 created`, `inline feature 18 evaluation succeeded`). |

Common cases:

* **"unknown technique" for DLSS5_Feed / your provider** — the shaders are not in
  `reshade-shaders\Shaders\`, or the runtime is D3D9 and they cannot compile (see
  [the D3D9 section](#install-for-a-directx-9-game-beta)).
* **Image is static-sharp but smears when moving** — no vectors are reaching DLSS. The overlay's
  **Motion vectors** section says which of the four causes it is: the provider is not installed,
  it is installed but **disabled**, it **failed to compile** (DRME on ReShade 6.8 — use LumeniteFX
  Kernel instead), or a *different* provider is enabled than the one `DLSS5_MV_PROVIDER` selects.
  The `MV probe … 0% non-zero` line in `dlss5-feed.log` confirms it independently.
* **Depth probe says sampled depth is flat** — open **DLSS 5 Feed – debug view**, select the depth
  view, and verify that scene geometry is visible. Then use ReShade's **Add-ons → Generic Depth**
  page to select the draw call/clear that contains the scene rather than UI or an already-cleared
  buffer. The warning is diagnostic only; it does not guess a different buffer or disable DLSS.
* **Subnautica: flat or wrong depth** — this 4K D3D11 profile was contributor-verified with DLAA +
  neural rendering. Merge these values into the matching sections of `ReShade.ini` and keep every
  unrelated key. In `PreprocessorDefinitions`, append or replace only the four definitions shown
  below in the existing comma-separated list; do not discard other definitions:

  ```ini
  [DEPTH]
  DepthCopyAtClearIndex=1
  DepthCopyBeforeClears=2
  DrawStatsHeuristic=0
  FilterFormat=0
  UseAspectRatioHeuristics=3

  [GENERAL]
  PreprocessorDefinitions=RESHADE_DEPTH_LINEARIZATION_FAR_PLANE=1000.0,RESHADE_DEPTH_INPUT_IS_UPSIDE_DOWN=1,RESHADE_DEPTH_INPUT_IS_REVERSED=1,RESHADE_DEPTH_INPUT_IS_LOGARITHMIC=0
  ```
* **Warping / smearing around flames, flickering lights or transparents** — optical flow answers a
  lighting change with a wrong-but-confident vector. Keep validation on (default), try provider
  `3` (LumeniteFX Kernel) rather than Launchpad, and try `preset=5` or `6` in `dlss5-feed.cfg`
  (the legacy CNN presets clamp history harder).
* **Nothing happens, no `dlss5-feed.log`** — ReShade's architecture does not match the game's
  (a 64-bit `dxgi.dll` cannot load into a 32-bit game, and vice versa).
* **"ran out of video memory" with dgVoodoo** — raise `VRAM` in `dgVoodoo.conf`; the default 256 MB
  is a virtual limit unrelated to your real GPU.
* **Vulkan game: "the Vulkan interop entry points are missing"** — the add-on's `vkCreateDevice`
  hook did not get to add the KHR external-interop extensions; the lines right above it in
  `dlss5-feed.log` say whether the hook was not installed, never reached, or what the driver
  refused. Fallback: launch via `layer\run-with-feed-layer.bat` (see
  [`layer/README.md`](layer/README.md)); `feed-vk-layer.log` next to the DLL shows what it added.
* **OpenGL game: "the OpenGL interop extensions are missing on the rendering GPU"** — the log line
  above it names the exact extension that was absent and prints `GL_RENDERER`. If that says anything
  other than an NVIDIA GPU, the game is rendering on the wrong adapter: force it onto the NVIDIA one
  (Windows **Settings ▸ Display ▸ Graphics**) and restart it. There is no fallback for this and
  there cannot be one — DLSS itself needs that GPU.
* **32-bit game: "the host64\ folder is from a different release"** — the add-on and the helper
  speak a versioned protocol (v2 added the OpenGL client kind, v3 the Vulkan one). Reinstall both
  halves from the same release rather than mixing them.
* **DLSS 5 panel stuck in STANDBY** — the add-on missed the first create; the built-in warm-up
  re-creates the feature a few seconds in, which normally clears it.
* **Neural rendering stops mid-session, no crash** — the overlay's **Status** section now names the
  reason next to `Session: disabled`, and **Re-enable** restarts it. If the log says `the GPU did
  not retire allocator slot N within 2000 ms`, the GPU is not keeping up rather than broken: raise
  `gpu_timeout_ms`. A single slow frame no longer stops the session — three consecutive failures do.
* **Corruption or flicker with Smooth Motion on** — see the Smooth Motion warning at the top of
  this README. The overlay says whether Smooth Motion was detected, and `dlss5-feed.log` records the feeding
  thread: `frame fed from thread N, not the usual M` means `Present` is arriving off-thread, and
  `re-entrant frame … dropped` means it arrived twice at once. Both lines are worth quoting on an
  issue. Turn Smooth Motion off for this game's API only, with Profile Inspector.

## Building

MSVC (v143/v145) + Windows SDK. Dependencies not vendored: the **NGX SDK** (see
[`external/ngx/README.md`](external/ngx/README.md)) and the **Vulkan headers** (see
[`external/vulkan/README.md`](external/vulkan/README.md)); the ReShade add-on headers *are* included
under `external/reshade/include` (BSD-3-Clause, Patrick Mours), as is **MinHook** under
`external/minhook` (BSD-2-Clause, Tsuda Kageyu) for the `vkCreateDevice` hook.

| Script | Output | Needs |
| --- | --- | --- |
| `build.bat` | `build\dlss5-feed.addon64` | NGX SDK |
| `build-addon32.bat` | `build\dlss5-feed.addon32` | Vulkan headers |
| `host\build-host.bat` | `host\dlss5-feed-host64.exe` | NGX SDK |
| `layer\build-layer.bat` | `layer\VkLayer_feed_vk.dll` and `layer\x86\VkLayer_feed_vk32.dll` (fallback for Vulkan games where the add-on's own `vkCreateDevice` hook cannot add the interop extensions; the 32-bit pair keeps its own subdirectory because the Vulkan loader tries every manifest on `VK_LAYER_PATH`) | Vulkan headers |
| `spike\build-spike.bat` | the standalone proofs used during development: the 32↔64-bit shared-resource pair, plus `spike-gl64.exe` / `spike-gl32.exe` and `spike-vkhost64.exe` / `spike-vkclient32.exe`, which round-trip a texture and a fence between D3D12 and OpenGL / Vulkan, in-process and cross-process. They need an NVIDIA GPU to *run*, none to compile. | — |

NGX links against the Release CRT, so the builds use `/MD`.

Each script picks up its toolchain through `toolscvars.bat`, which asks `vswhere` for the latest
Visual Studio install with the C++ tools and falls back to a fixed BuildTools path. Set `VCVARSALL`
to your own `vcvarsall.bat` to override it.

**CI** — `.github/workflows/build.yml` builds all five targets on every pull request, fetching the
NGX SDK and the Vulkan headers from their upstream repositories, and uploads the binaries as a
workflow artifact. It only proves the tree compiles and links: DLSS needs an RTX GPU and a real
swapchain, so nothing in the table under [Status](#status) can be verified there.

## Limitations and roadmap

* **DLAA contract, optional reduced work extent on 64-bit D3D11** — render resolution still
  equals DLAA output resolution, but the private work extent can be 50–100% of the native
  backbuffer and is spatially expanded afterward. D3D12, Vulkan, OpenGL and 32-bit paths remain at
  100%. This is not jittered DLSS Super Resolution.
* Estimated motion vectors → temporal artifacts in fast motion; the UI is processed with the scene
  (a UI mask / pre-UI colour capture is future work).
* **Geometry vectors are experimental and off.** The camera-model fit is derived from the provider's
  own flow, so it inherits that noise, and it has no way to know the HUD is not part of the scene.
  Doing it properly needs the game's real view-projection matrices, not a fit.
* A light that flickers faster than DLSS's history converges is averaged into a slow pulse. The
  trust mask reduces it; nothing available to a post-process feed removes it.
* Exclusive-fullscreen swapchain churn can make some games reload effects repeatedly; windowed is
  smoother.
* Depends on a closed-source, community-distributed DLSS 5 add-on and the NGX runtime; both can change.
* The **32-bit and D3D9 paths are beta** — see [`PLAN-32BIT.md`](PLAN-32BIT.md) for the full design
  and known risks. Cross-process adds a small amount of scheduling jitter versus the in-process
  64-bit path (not measured as a problem so far).
* **32-bit Vulkan has not run in a real game yet** — the cross-bitness interop is proven by the
  spike pair, but nothing above it is. See [`PLAN-VULKAN32.md`](PLAN-VULKAN32.md).

## Credits

* **D3D11↔D3D12 shared-texture / fence transport** adapted from NIGos'
  [dlss5-dx11-bridge](https://github.com/NIGos/dlss5-dx11-bridge) (MIT) — not re-hosted here.
* **Motion vectors:** interop happens purely by declaring each provider's output texture
  identically, so ReShade binds the same resource — the mechanism `dh_uber_rt` and VORT use. Thanks
  to **[LumeniteFX](https://github.com/umar-afzaal/LumeniteFX)** (Umar Afzaal), **iMMERSE
  Launchpad** (MartysMods), **VORT** (MIT), Jakob Wapenhensch's
  [ReshadeMotionEstimation](https://github.com/JakobPCoder/ReshadeMotionEstimation) (CC BY-NC 4.0),
  the qUINT ecosystem that established the `texMotionVectors` convention, and
  [AlucardDH's dh-reshade-shaders](https://github.com/AlucardDH/dh-reshade-shaders) for the
  provider-switch pattern. **No provider's files are bundled or included by this project's shader**
  — install them from their own repositories, under their own licenses.
* **DLSS 5 neural rendering:** the RenoDX community's `renodx-dlss5` add-on.
* **ReShade** add-on API by Patrick Mours.
* **dgVoodoo2** by Dege — the D3D9 translation layer that makes the DirectX 9 path possible.
* **D3D12 stability findings** independently confirmed by the
  [Pizzawookiee fork](https://github.com/Pizzawookiee/DLSS5-Feeder)'s diagnostics.

## License

MIT — see [LICENSE](LICENSE). This covers only the code in this repository (`src/`, `shaders/`,
`host/`); the dependencies above keep their own licenses.
