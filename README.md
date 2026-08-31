## ⚠️ Not compatible with Nvidia Smooth Motion / Optiscaler. Disable them to avoid issues.

# DLSS5-Feeder

**DLSS 5 neural rendering in games that ship without any DLSS — D3D11, D3D12, Vulkan, 32-bit, even DirectX 9.**

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
- [Motion vectors: choosing a provider](#motion-vectors-choosing-a-provider)
- [How it works](#how-it-works)
  - [The 32-bit path](#the-32-bit-path)
  - [The DirectX 9 path](#the-directx-9-path)
  - [The Vulkan path](#the-vulkan-path)
- [Requirements](#requirements)
- [Configuration](#configuration)
- [Logs and troubleshooting](#logs-and-troubleshooting)
- [Building](#building)
- [Limitations and roadmap](#limitations-and-roadmap)
- [Credits](#credits)
- [License](#license)

## Status

Proven working in six games covering every supported path:

| Game | Bitness / API | Result |
| --- | --- | --- |
| **Metro 2033 Redux** | 64-bit D3D11 | 4K DLAA + NR, HDR backbuffer |
| **The Lord of the Rings: War in the North – Legacy Edition** | 64-bit D3D12 | 4K, same-device path, 120 fps |
| **Splinter Cell: Blacklist** | 32-bit D3D11 | 60 fps, cross-process host |
| **BioShock Remastered** | 32-bit D3D11 (D3D9→D3D11 wrapper) | 4K, Luma HDR |
| **Fable Anniversary** | 32-bit **D3D9** via dgVoodoo2 | 1440p, 60 fps |
| **DOOM (2016)** | 64-bit **Vulkan** | 4K, D3D12 evaluate via cross-API interop |

In each, the DLSS 5 add-on reports `feature 18 created … inline feature 18 evaluation succeeded`,
driven entirely by ReShade depth + estimated motion vectors.

It is not game-specific: any D3D11, D3D12 or Vulkan game with a working ReShade depth buffer and a
motion vector provider should work — 64-bit directly, 32-bit via a bundled 64-bit helper process,
D3D9 via a wrapper.

**This is beta software.** Expect the temporal quality of *estimated* motion vectors (some ghosting
in fast motion, softness on thin moving geometry), and the HUD is processed along with the scene.

> ### 0.6.0-beta: read this before installing
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

1. **ReShade with add-on support** — from **https://reshade.me**, run the installer, pick your game's
   `.exe`, choose **Direct3D 10/11/12**, and tick **"Enable loading of add-ons"** (the full /
   unsigned build). This puts `dxgi.dll` next to the game.
2. **DLSS5-Feeder** — from the
   **[latest release](https://github.com/jlrouzies-fr/DLSS5-Feeder/releases/latest)** download
   **`dlss5-feed.addon64`** and **`DLSS5_Feed.fx`**. Put `dlss5-feed.addon64` next to the game `.exe`
   (same folder as `dxgi.dll`), and `DLSS5_Feed.fx` into `reshade-shaders\Shaders\`.
3. **Motion vectors** — the recommended provider is
   **[LumeniteFX](https://github.com/umar-afzaal/LumeniteFX)** (its own repository; nothing of it
   is bundled here). Green **Code ▸ Download ZIP**, then copy
   - everything in its `Shaders\` — the `lumenite_*.fx` files **and** the `include\` folder —
     into `reshade-shaders\Shaders\`
   - `Textures\lumenite_bluenoise256.png` into `reshade-shaders\Textures\`

   Four other providers are supported; see
   [Motion vectors: choosing a provider](#motion-vectors-choosing-a-provider).
   *(Our shader only reads the provider's output texture — it includes no third-party shader files.)*
4. **DLSS 5 neural-rendering add-on** — `renodx-dlss5.addon64` and its model `nvngx_dlssnr.dll`.
   Easiest is the **RHI** installer, which downloads and deploys them for you:
   **https://github.com/RankFTW/RHI/releases** (or get them from the RenoDX Discord). Put both next
   to the game `.exe`. Also drop a **`nvngx_dlss.dll`** there (any DLSS game has one, or use
   [DLSS Swapper](https://github.com/beeradmoore/dlss-swapper)).
5. **Point the shader at that provider** — press **Home** for the ReShade overlay, select
   `DLSS5_Feed.fx`, and in its **Preprocessor definitions** set **`DLSS5_MV_PROVIDER` = `3`**
   (LumeniteFX Kernel), then reload effects. The shader's panel then reads
   *"Motion vector provider: LumeniteFX Kernel"*.
6. **Turn it on in-game** — enable **"LUMENITE: Kernel 2.0"**, then enable **DLSS 5 Feed**
   *below it*, and enable neural rendering in the **DLSS 5 Neural Rendering** panel. Keep the
   game's MSAA/SSAA **off**.

Check `dlss5-feed.log` (next to the game `.exe`) for `feature ready … DLAA`, `frame N delivered`,
and `DLSS5_MV_PROVIDER=3 (LumeniteFX Kernel) -> Lumenite_Kernel (enabled)`. The overlay's
**Motion vectors** section shows the same, in red if the shader and the enabled provider disagree.
`dlss5-feed.cfg` is created automatically with working defaults.

> **Do I need the DLSS 5 DX11 *bridge*?** **No.** DLSS5-Feeder does the bridge's job for games that
> have no DLSS. The bridge — **https://github.com/NIGos/dlss5-dx11-bridge/releases** — is only for
> DX11 games that *already* have their own DLSS; don't run both for the same game.

## Install for a 32-bit game (beta)

NGX and the DLSS 5 add-on are both 64-bit-only, so on a 32-bit game DLSS5-Feeder splits in two: a
tiny 32-bit add-on lives in the game and ships frames to a bundled 64-bit helper process, which does
all the actual DLSS/NGX work (details in [The 32-bit path](#the-32-bit-path)).

1. **ReShade with add-on support** — as above, but the installer must detect your game as **32-bit**
   and install the x86 build. (`dxgi.dll` should be ~4.4 MB; check its file properties if unsure.)
2. **DLSS5-Feeder for 32-bit** — from the
   **[latest release](https://github.com/jlrouzies-fr/DLSS5-Feeder/releases/latest)** download
   **`dlss5-feed.addon32`**, **`DLSS5_Feed.fx`** and **`dlss5-feed-host64.exe`**.
   - `dlss5-feed.addon32` → next to the game `.exe`
   - `DLSS5_Feed.fx` → `reshade-shaders\Shaders\`
   - create a **`host64\`** folder next to the game `.exe` and put `dlss5-feed-host64.exe` in it
3. **Fill `host64\` with the 64-bit pieces** — the helper is a self-contained "game" of its own and
   needs its **own** copies: a 64-bit ReShade `dxgi.dll`, `renodx-dlss5.addon64`, `nvngx_dlssnr.dll`
   and `nvngx_dlss.dll`. (Run the ReShade installer once against any 64-bit game to obtain the x64
   `dxgi.dll`, or extract `ReShade64.dll` from the installer and rename it.)
4. **Motion vectors** — a provider into the game's `reshade-shaders\`, and the matching
   `DLSS5_MV_PROVIDER` definition, exactly as in steps 3 and 5 of the 64-bit instructions.
5. **Turn it on in-game** — as above, and check **ReShade's overlay → Add-ons tab → DLSS 5 Feed**:
   the day-to-day DLSS 5 settings (neural uplift, NR intensity/style, …) are right there with an
   **Apply** button — see [Configuration](#configuration). Set `host_window=0` on that page once
   you are happy, since you should rarely need the separate window below.

The first fed frame also spawns `host64\dlss5-feed-host64.exe`, which opens a window titled
**"32-bit DLSS 5 Feeder"** — the add-on and the game never share a ReShade instance, so this is
where the DLSS 5 add-on's *own* full panel lives, for anything not covered by our overlay page.
Press Home in that window to open it:

<img width="1880" height="1058" alt="image" src="https://github.com/user-attachments/assets/57abd732-94d2-401c-a524-6536006f3c86" />

## Install for a DirectX 9 game (beta)

A real D3D9 device cannot work directly: ReShade on D3D9 caps at Shader Model 3, so **no** motion
vector provider (ReshadeMotionEstimation, qUINT, … — all SM5) can even compile, and D3D9 has
no shared NT handles or fences for the transport. The fix is to translate D3D9 to D3D11 first with
**[dgVoodoo2](http://dege.freeweb.hu/dgVoodoo2/)**, which turns the game into the ordinary supported
case — SM5 shaders, shareable textures, real fences.

**Confirm you need this first:** run the game once with ReShade installed and look in `ReShade.log`.
If the *runtime* is created after `Redirecting Direct3DCreate9` and you see `IDirect3DDevice9`, it is
a genuine D3D9 game. (If instead you see `D3D11CreateDevice` and `Using feature level b000/b100`, the
game already wraps to D3D11 — skip this section, it is just a normal 32-bit install.)

1. **Download dgVoodoo2** from **http://dege.freeweb.hu/dgVoodoo2/** and unzip it.
2. **Copy three files next to the game's `.exe`** — this is the folder containing the executable,
   which is often *not* the game's root folder (Fable Anniversary's is `Binaries\Win32\`):
   - `MS\x86\D3D9.dll` — from the **`MS`** folder (DirectX), and **`x86`** for a 32-bit game
     (`x64` only if the game is 64-bit)
   - `dgVoodoo.conf`
   - `dgVoodooCpl.exe`
3. **Configure it.** Run `dgVoodooCpl.exe` **from that folder** (it only edits the `dgVoodoo.conf`
   sitting beside it), or edit `dgVoodoo.conf` directly. In the **`[DirectX]`** section:

   | Setting | Value | Why |
   | --- | --- | --- |
   | `DisableAndPassThru` | **`false`** | **The shipped default is `true`**, which makes dgVoodoo forward everything to the real D3D9 and do nothing at all. This is the single most common reason "dgVoodoo doesn't seem to do anything". |
   | `VRAM` | **`1GB`** | The default `256` MB is a *virtual* card size reported to the game and causes **"ran out of video memory"** crashes regardless of your real GPU. Do not use `2GB`: 2048 MB in bytes is `0x80000000`, which overflows a signed 32-bit integer and old engines mishandle it. |
   | `VideoCard` | `internal3D` | dgVoodoo's own virtual card; exposes the most capabilities. |
   | `dgVoodooWatermark` | `true` | Temporarily — it is your proof dgVoodoo is actually running. |

   And in **`[General]`**: `OutputAPI = d3d11_fl11_0` (or higher).
4. **Verify before going further.** Launch the game: the **dgVoodoo watermark must appear**. If it
   does not, dgVoodoo is not loading (wrong folder, wrong architecture, or `DisableAndPassThru` is
   still `true`) and nothing else will work. `ReShade.log` should now show `D3D11CreateDevice`,
   `Using feature level b000`, and `Recreated runtime environment` — a real D3D11 runtime.
5. **Install DLSS5-Feeder normally** — follow
   [Install for a 32-bit game](#install-for-a-32-bit-game-beta) (or the 64-bit steps for a 64-bit
   D3D9 game). ReShade must be installed as **`dxgi.dll`**, never as `d3d9.dll` — dgVoodoo owns that
   filename now and the two would fight.
6. Turn the watermark off once everything works.

## Install for a Vulkan game

Same pieces as a 64-bit game — with two differences.

1. **ReShade for Vulkan is a layer, not a `dxgi.dll`.** Its installer registers it globally and
   gates it per-application, so make sure your game's exe is in that list (ReShade's installer adds
   it when you point it at the exe). The add-ons still go next to the game exe, and the game's
   `ReShade.ini` needs `AddonPath=.\` under `[ADDON]` so they are found there.
2. **Everything else is identical** — `dlss5-feed.addon64`, `DLSS5_Feed.fx`, a motion-vector
   provider, `renodx-dlss5.addon64` + the `nvngx_*.dll` files, exactly as in the
   [64-bit instructions](#install-for-a-64-bit-game). The DLSS evaluate runs on a private D3D12
   device (see [The Vulkan path](#the-vulkan-path)); nothing extra is needed for that.

The transport needs the KHR external-interop extensions on the game's `VkDevice`, and most games do
not enable them. **The add-on takes care of that by itself**: ReShade loads add-ons from inside its
`vkCreateInstance` hook, i.e. before the game creates its device, so the add-on hooks
`vkCreateDevice` and appends whichever of those extensions the driver supports (plus the
`timelineSemaphore` feature). `dlss5-feed.log` lists, per extension, whether the game already had
it, the add-on added it, or the driver lacks it. If the driver refuses the extended list, the call
is retried untouched — the hook can never stop a game from starting.

**Only if `dlss5-feed.log` still says the interop entry points are missing** (the log says why:
hook not installed, or installed but never reached — a game that creates its device some way the
hook does not intercept), launch through the bundled out-of-process layer instead:

```
layer\run-with-feed-layer.bat "E:\path\to\game.exe"
```

See [`layer/README.md`](layer/README.md). It does the same job from outside the process.

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

### Are the vectors actually arriving?

Two independent checks, both new in 0.6.0:

* The overlay's **Motion vectors** section states the mode, the provider found, and its state
  (`enabled` / `DISABLED` / `FAILED TO COMPILE` / `not installed`), in red when something is wrong.
* An **MV probe** reads back the vectors *actually handed to DLSS* every 600 frames and logs their
  mean/max magnitude and non-zero share: `MV probe … N% non-zero`. While you move, that must not
  be 0%.

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
  in `DLSS5_Mask` (R8).
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

## Requirements

| Piece | Notes |
| --- | --- |
| D3D11, D3D12 or Vulkan game, 32- or 64-bit | NGX is 64-bit only, hence the helper process for 32-bit games. D3D9 works through [dgVoodoo2](#install-for-a-directx-9-game-beta); Vulkan works out of the box (the add-on adds the interop extensions itself; [a small bundled layer](#install-for-a-vulkan-game) is the fallback). D3D10 is not supported. |
| ReShade 6.8+ **with add-on support** | Generic Depth add-on enabled and picking the scene depth. |
| DLSS 5 neural-rendering add-on (`renodx-dlss5.addon64`) + `nvngx_dlssnr.dll` | from its own author; this project does not include it. |
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
  image, which test fired, and the three geometry views.

Preprocessor definitions on the shader (overlay → *Preprocessor definitions* → reload effects):

| Definition | Default | Meaning |
| --- | --- | --- |
| `DLSS5_MV_PROVIDER` | `0` | Which provider's output to read — see [the provider table](#motion-vectors-choosing-a-provider). |

## Logs and troubleshooting

| File | Contents |
| --- | --- |
| `dlss5-feed.log` | next to the game exe: resolved effect handles, the session, the contract (`feature ready: WxH DLAA, flags=…`), `frame N delivered`, timing every 600 frames, crash breadcrumbs. |
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
* **DLSS 5 panel stuck in STANDBY** — the add-on missed the first create; the built-in warm-up
  re-creates the feature a few seconds in, which normally clears it.

## Building

MSVC (v143/v145) + Windows SDK. Dependencies not vendored: the **NGX SDK** (see
[`external/ngx/README.md`](external/ngx/README.md)) and the **Vulkan headers** (see
[`external/vulkan/README.md`](external/vulkan/README.md)); the ReShade add-on headers *are* included
under `external/reshade/include` (BSD-3-Clause, Patrick Mours), as is **MinHook** under
`external/minhook` (BSD-2-Clause, Tsuda Kageyu) for the `vkCreateDevice` hook.

| Script | Output | Needs |
| --- | --- | --- |
| `build.bat` | `build\dlss5-feed.addon64` | NGX SDK |
| `build-addon32.bat` | `build\dlss5-feed.addon32` | ReShade headers only |
| `host\build-host.bat` | `host\dlss5-feed-host64.exe` | NGX SDK |
| `layer\build-layer.bat` | `layer\VkLayer_feed_vk.dll` (fallback for Vulkan games where the add-on's own `vkCreateDevice` hook cannot add the interop extensions) | Vulkan headers |
| `spike\build-spike.bat` | the standalone 32↔64-bit shared-resource proof used during development | — |

NGX links against the Release CRT, so the builds use `/MD`.

## Limitations and roadmap

* **DLAA contract, optional reduced work extent on 64-bit D3D11** — render resolution still
  equals DLAA output resolution, but the private work extent can be 50–100% of the native
  backbuffer and is spatially expanded afterward. D3D12, Vulkan and 32-bit paths remain at
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
