![DLSS5 Feeder](dlss5-feeder-logo-dark.png)

[![AI-DECLARATION: copilot](https://img.shields.io/badge/䷼%20AI--DECLARATION-copilot-fee2e2?labelColor=fee2e2)](AI-DECLARATION.md)

## Description

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

**[↓ Jump to the Contents](#contents)**

---

## Before you install: three things

None of this is hard, and the [automated installer](#install-the-automated-way) verifies most of it for you.

### 1. You might not need this project at all

If your game is **64-bit** and uses **DirectX 9, 11 or 12**, there is now a simpler option:
ShortFuse's **renodx-dlss** add-on does the whole job by itself. Use that instead — it is one
add-on rather than two, and there is nothing for this project to add.

| Your game | What to use |
| --- | --- |
| 64-bit, DirectX 9 / 11 / 12 | **renodx-dlss** on its own — you do not need DLSS5-Feeder |
| **32-bit** (any graphics API) | **DLSS5-Feeder** |
| **Vulkan** | **DLSS5-Feeder** |
| **DirectX 9**, and you want the best handling of motion | **DLSS5-Feeder** |

renodx-dlss is not on GitHub. It comes from the RenoDX Discord, `#DLSS5` channel:
<https://discord.com/invite/renodx>

<details>
<summary>Why DLSS5-Feeder is still the only option for those three</summary>

- **32-bit games.** renodx-dlss is 64-bit only, and NVIDIA ships no 32-bit NGX runtime at all, so
  an in-process approach is impossible there by construction. This project's cross-process helper
  is the only route.
- **Vulkan games.** Covered here through the bundled layer.
- **Real motion vectors on D3D9.** renodx-dlss evaluates only the finished backbuffer there, with
  no temporal inputs. This project drives a full temporal evaluate from ReShade motion vectors.

</details>

### 2. You need one neural add-on installed next to this one

DLSS5-Feeder does not sharpen anything by itself. It builds the DLSS request your game never
makes; a **second add-on** does the actual neural rendering. You install **exactly one** of them.

**Recommended: Deep Fried Chicken**, by Alexander, from its Discord:
<https://discord.gg/g2v2XGqvR> — take **1.4.8 or newer**.

Copy its three files next to your game's `.exe` (or into `host64\` for a 32-bit game):
`deep-fried-chicken.addon64`, `deep-fried-chicken-nvngx.dll` and `deep-fried-chicken.cfg`.
Its settings file already arrives set up correctly, so leave it alone for your first run. You
supply `nvngx_dlssnr.dll` and `nvngx_dlss.dll` yourself, as always.

> **Never install two neural add-ons.** If Deep Fried Chicken finds RenoDX's add-on or Alex's
> Toolkit loaded beside it, it does nothing at all for the whole session — silently, as far as
> your eyes are concerned. Pick one. If something looks like it is not working, this is the first
> thing to check.

<details>
<summary>Using Krish's RenoDX add-on instead (the older option, still fully supported)</summary>

`renodx-dlss5.addon64`, the `#DLSS5` build, from the RenoDX Discord, `#DLSS5` channel:
<https://discord.com/invite/renodx>. It is what every release
up to now was developed against.

The feeder fingerprints whichever build it finds and adapts, so any recent one works — take the
newest in the channel. Any `renodx-dlss5*.addon64` name is recognised (`renodx-dlss5-4.7.addon64`
included; before 0.11.0-beta.2 only the exact name was, and a versioned file was logged as "not
found" and treated as the classic engine). Keep just one copy — ReShade loads them all.

The feeder writes `EnableHooks=2`, `NeuralUplift=1` and `NREnableUpscaling=0` when they are unset,
and in the `host64` helper it unbinds the global hotkeys so a gameplay keypress cannot silently
toggle neural rendering in a background process. Every guard is keyed to a marker only the build
that needs it carries. **None of this applies to Deep Fried Chicken**, which has its own config
file and its own overlay tab.

| Build | Marker | What the feeder does about it |
| --- | --- | --- |
| v4.7 | `NRGlobalTone` | Newest, checked 2026-09-01. Replaces the paper-white codec with a reversible colour bridge (SDR sRGB / linear HDR BT.709 / PQ BT.2020) picked from the contract the feeder already publishes, plus a fenced D3D12 workset pool. Nothing new is required from the feeder; the 32-bit overlay mirrors its renamed sliders and its two new keys. |
| v4.6 | `NRToggleKey` | Global hotkeys, WIP upscaling with a rejection latch, richer decline diagnostics. See the `NRStyle=2` note under Smooth Motion below. |
| v45+ | `EnableHooks` | Rescans every present and adopts missed features lazily, so the feeder skips its warm-up re-create. |
| older | — | Classic single hook pass; the warm-up re-create stays on. |

**Verification status:** no game row has verified a v4.6 or v4.7 run end-to-end yet. The
compatibility work is static — marker detection, key defaults, panel mirroring — and both
generations were checked against the shipped binaries.

</details>

<details>
<summary>Careful: renodx-dlss and renodx-dlss5 are two different add-ons</summary>

- **`renodx-dlss5`** (Krish) is a *neural add-on*. It works **with** this project, as the
  alternative to Deep Fried Chicken above.
- **`renodx-dlss`** (ShortFuse) *replaces* this project — it builds the DLSS request itself, so it
  neither needs nor tolerates the feeder. One or the other, never both.

</details>

### 3. Turn off OptiScaler, and Smooth Motion on Vulkan

- **OptiScaler** — turn it off. It drives the same upscaling path this project does, and the two
  fight over it.
- **NVIDIA Smooth Motion** — turn it off **for Vulkan games only**. On Vulkan the two cannot work
  together, and no future release can fix that. You would see roughly half your frames
  unprocessed, which looks like heavy flickering or a picture stuck on an old frame. Nothing is
  broken and nothing is crashing; it simply cannot work.
- **DirectX 11 and 12 games are fine.** Leave Smooth Motion on there if you like it.

To switch it off for Vulkan alone and keep it everywhere else: in **NVIDIA Profile Inspector**, set
**Smooth Motion - Enabled APIs** (`0xB0CC0875`) and clear the Vulkan bit — the value is a sum of
`1` for DX12, `2` for DX11 and `4` for Vulkan, so `7` becomes `3`.

<details>
<summary>Why Vulkan cannot work, and how to check whether Smooth Motion is on</summary>

Smooth Motion doubles your frame rate by inventing an extra frame between each pair of real ones.
On Vulkan it does that **inside the graphics driver** — the very last step before the picture
reaches your monitor, past anything ReShade or any add-on can touch.

This project does its work earlier, while the frame is still being built. So the frames the game
really renders get neural rendering applied correctly, but the frames the driver *invents* are
built from a copy taken before we ran, and carry none of it. Half the frames you see are processed
and half are not. The log will happily report every frame delivered, because from the feeder's
point of view they were.

We tried to close that gap and could not, which is why this is documented rather than fixed.
Across five rebuilds on DOOM 2016 — a game that works perfectly with Smooth Motion off — we
confirmed the feeder's work is correct and complete before the frame is handed over, that it can
finish early enough, and that it can be made structurally identical to a normal game's frame. The
invented frames still never carried our output. The driver simply does not read what we wrote.
Full detail in [`PLAN-DETROIT.md`](PLAN-DETROIT.md).

**The feeder cannot warn you about this on Vulkan.** Its Smooth Motion check looks for a file the
driver only loads for DirectX games, so a silent log does **not** mean Smooth Motion is off. To be
certain, turn on **Smooth Motion - Debug Bars** (`0xB01B8B02`) in Profile Inspector and launch:
coloured bars on screen mean it is running. (0.9.0 and newer also watch for the tell-tale extra
frames and warn when they spot them, but the Debug Bars check is the reliable one.)

| Profile Inspector setting | ID | Value |
| --- | --- | --- |
| Smooth Motion - Enabled APIs | `0xB0CC0875` | bitfield, default `7`: `1` DX12, `2` DX11, `4` Vulkan — clear only the bit for this game |
| Smooth Motion - Enable | `0xB0D384C0` | `0` / `1`, per application |
| Smooth Motion - Debug Bars | `0xB01B8B02` | draws coloured bars on generated frames — use it to check whether the bad frames *are* the generated ones |

</details>

<details>
<summary>Smooth Motion on DirectX 11 / 12: what changed in 0.11.0-beta.2</summary>

It should work from 0.11.0-beta.2 on. Smooth Motion adds its own D3D11 device and an invisible
proxy swapchain (`InvisibleWindowClassNvPresent`), and ReShade creates an effect runtime on
**each** — `ReShade.ini` for one, `ReShade2.ini` for the other. Until beta.2 the feeder fed
whichever runtime initialised last, which under Smooth Motion was routinely the one *without* your
preset: the log looked healthy and nothing was neural (issue
[#1](https://github.com/jlrouzies-fr/DLSS5-Feeder/issues/1), Ghost Recon Breakpoint and AC
Syndicate). It now tracks every runtime and feeds the one that actually renders `DLSS5_Feed`;
`dlss5-feed.log` lists each runtime with its device and window class, and says which one is bound.
Since 0.8.0 it is also thread-safe against Smooth Motion's extra `Present` calls.

If the picture still shows no effect with Smooth Motion on, post the log after a minute of play
(not the first seconds) on #1, and say whether your *other* ReShade effects are visible.

</details>

<details>
<summary>Two other add-ons Smooth Motion is known to crash (neither is this one)</summary>

Both crash the game 1–2 s into boot with a null read on the present path, before the feeder feeds
a frame. Found during a Metro 2033 Redux run.

- **Luma** (`Luma-Metro Redux.addon`): reads `GetCurrentBackBufferIndex()` and passes it to
  `GetBuffer()`, which fails under Smooth Motion's flip-model wrapper; the null back buffer is then
  dereferenced. Fix offered upstream to Filoppi/Luma-Framework. Remove the Luma add-on to boot, or
  turn Smooth Motion off for that game.
- **`NRStyle=2`** (the RenoDX add-on's own setting, v4.6+, from its overlay panel): crashes at the
  next boot even without Luma. The feeder warns when it sees it; set `NRStyle=0` in `ReShade.ini`'s
  `[RenoDX.DLSS5]` section to recover.

</details>

## Contents

- [Before you install: three things](#before-you-install-three-things)
- [Status](#status)
- [Install: the automated way](#install-the-automated-way)
- [Install for a 64-bit game](#install-for-a-64-bit-game)
  - [Deep Fried Chicken: first run](#deep-fried-chicken-first-run)
  - [Alternative: the RenoDX add-on](#alternative-the-renodx-add-on)
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
- [AI declaration](AI-DECLARATION.md) — what was written by AI, and what wasn't

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
| **Guild Wars Reforged** | 32-bit **Vulkan** via DXVK | User-reported (#32): 3440x1440, 55 fps on an RTX 4070 (350+ without) |
| **World of Warcraft 3.3.5a** | 32-bit **Vulkan** via DXVK | User-reported (#15): 4K on an RTX 5080/5090; the working `dxvk.conf` is in the DXVK section |
| **Star Wars: KOTOR** | 32-bit **OpenGL** | User-reported (#31): RTX 2060, fixed in 0.9.0 (capped `GL_EXTENSIONS` string) |

In each, the DLSS 5 add-on reports `feature 18 created … inline feature 18 evaluation succeeded`,
driven entirely by ReShade depth + estimated motion vectors.

It is not game-specific: any D3D11, D3D12, Vulkan or OpenGL game with a working ReShade depth buffer
and a motion vector provider should work — 64-bit directly, 32-bit via a bundled 64-bit helper
process, D3D9 via a wrapper.

**Detroit: Become Human (Demo), 64-bit Vulkan — almost certainly the Smooth Motion case above.**
The picture holds stale frames while the log reports every frame delivered, which is exactly the
signature described in the Smooth Motion warning. Enabling Smooth Motion in DOOM 2016 reproduces
it in a game that otherwise works, and the feeder cannot detect Smooth Motion on Vulkan, so its
silent log never ruled it out here. **If you hit this: check "Smooth Motion - Debug Bars"
(`0xB01B8B02`), and if the bars appear, turn Smooth Motion off for Vulkan.** Eight further tests
(DLSS/NGX, renodx NR, HDR and format detection, cross-API memory coherence, queue-family
ownership) all came back clean — full investigation in [`PLAN-DETROIT.md`](PLAN-DETROIT.md).

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

## Install: the automated way

`Install-DLSS5Feeder.ps1` does the whole install from one command:

- Download [the script](tools/Install-DLSS5Feeder.ps1).
- Drop the `.ps1` file next to your game's `.exe`.
- Right-click in the folder ▸ **Open in Terminal**.
- Paste this and press Enter:
  ```
  powershell.exe -ExecutionPolicy Bypass -File .\Install-DLSS5Feeder.ps1
  ```
- Confirm the executable it proposes.

![Installer](dlss5-feeder-install-script.png)

Or point it at the exe from anywhere:

```
powershell -ExecutionPolicy Bypass -File .\Install-DLSS5Feeder.ps1 "C:\path\to\game.exe"
```

It reads the architecture and render API out of the executable, then downloads (once, into a
cache) and installs everything the manual sections below describe: ReShade 6.8+ with add-on
support (a local DLL, or the machine-wide Vulkan layer plus the `ReShadeApps.ini` entry), the
feeder from the latest release (`addon32` + `host64\` for a 32-bit game), the ReShade framework
headers, LumeniteFX, Deep Fried Chicken, both NVIDIA runtimes, dgVoodoo2 for Direct3D 8/9 games
(watermark off; `-DgVoodooWatermark` keeps it), and `ReShade.ini` / `ReShadePreset.ini` with the
provider selected and both techniques enabled in the right order. Existing files are merged into
and backed up, not replaced. It ends by running a read-only verification pass and prints the
result — every line is `[ OK ]`, `[WARN]` or `[FAIL]`, with the fix for each failure.

Things to know:

- **Windows Defender flags Deep Fried Chicken** (it hooks NVIDIA's NGX runtime with Detours,
  which heuristics dislike). The script tries the plain install first; only if Defender removes
  the file does it explain, ask, and add an exclusion for the game folder through a UAC prompt.
  Nothing is excluded without your yes.
- Vulkan games need one UAC prompt to register ReShade's layer and add the exe to
  `ReShadeApps.ini`. `-NoElevate` turns every such step into printed instructions instead.
- **It asks which neural consumer you want** before downloading anything: Deep Fried Chicken,
  or Krish's RenoDX DLSS 5 add-on. Both are fetched automatically, only one is ever installed,
  and if the other is already in the folder it offers to disable it. `-Consumer DFC` or
  `-Consumer RenoDX` answers that in advance for an unattended run.
- Pieces you already have go in a folder passed with `-LocalFiles`, or one at a time with
  `-DfcZip`, `-RenoDxAddon`, `-DlssNrDll`, `-DlssDll`, `-FeederZip`, `-ReShadeSetup`,
  `-LumeniteZip`, `-DgVoodooZip`.
- `-Api D3D|Vulkan|OpenGL|D3D9|D3D8` overrides the detection (some engines, Max Payne 3 among
  them, can run on either Direct3D 9 or 11; the script assumes 11 and says so).
- The Discord download links inside the script expire; when one does, the script says so and
  tells you which parameter takes a fresh link or file.

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
4. Get the neural consumer — **[Deep Fried Chicken](https://discord.gg/g2v2XGqvR)**
   (see [Before you install](#2-you-need-one-neural-add-on-installed-next-to-this-one)) — and put its three files next to the game `.exe`:
   `deep-fried-chicken.addon64`, `deep-fried-chicken-nvngx.dll` and `deep-fried-chicken.cfg`.
   Add **`nvngx_dlssnr.dll`** (from the RenoDX Discord — Chicken does not bundle it) and a
   **`nvngx_dlss.dll`** (from any DLSS game, or
   [DLSS Swapper](https://github.com/beeradmoore/dlss-swapper)) in the same folder.
   *(Using the RenoDX add-on instead? See [Alternative: the RenoDX add-on](#alternative-the-renodx-add-on).)*
5. Press **Home** for the ReShade overlay, select `DLSS5_Feed.fx`, set **`DLSS5_MV_PROVIDER` = `3`**
   in its Preprocessor definitions, and reload effects.
6. In-game: enable **"LUMENITE: Kernel 2.0"**, then **DLSS 5 Feed** below it, then turn on neural
   rendering in the consumer's own panel — the **Deep Fried Chicken** tab, or the **DLSS 5 Neural
   Rendering** panel if you took the RenoDX route. Turn the game's MSAA/SSAA **off**.

Check `dlss5-feed.log` (next to the game `.exe`) for `feature ready … DLAA`, `frame N delivered`,
and `DLSS5_MV_PROVIDER=3 (LumeniteFX Kernel) -> Lumenite_Kernel (enabled)`. The overlay's
**Motion vectors** section shows the same, in red if the shader and the enabled provider disagree.
`dlss5-feed.cfg` is created automatically with working defaults.

> **Do I need the DLSS 5 DX11 *bridge*?** **No.** DLSS5-Feeder does the bridge's job for games that
> have no DLSS. The bridge — **https://github.com/NIGos/dlss5-dx11-bridge/releases** — is only for
> DX11 games that *already* have their own DLSS; don't run both for the same game.

### Deep Fried Chicken: first run

**Deep Fried Chicken** (by Alexander) is the add-on that does the neural rendering on top of what
this project feeds it. There is nothing to set up on our side — the two recognise each other
automatically.

Four things to know:

1. **Don't edit its settings file.** The `deep-fried-chicken.cfg` it ships with is already the
   right starting point: on, one pass, extras off.
2. **It will ask you to restart once more.** On its first run it adds itself to ReShade's startup
   list and wants one more restart. That is normal, and only happens once.
3. **Start with one pass.** It can do up to 30. That is a stress test, not a starting point.
4. **Nothing else neural in the folder.** No RenoDX add-on, no Alex's Toolkit, no DX11 bridge. With
   any of those present, Chicken quietly does nothing at all.

For a **32-bit game**, its three files go in the `host64\` folder, next to `dlss5-feed-host64.exe` —
not beside the game itself. A 32-bit game cannot load them.

**Is it working?** The overlay's **Status** section and `dlss5-feed.log` both show Chicken's version
and its state. **`ARMED` is what you want.** `DISARMED`, `CONFLICT` or `FAILED` mean it is not
running its passes, and the log says which and why.

<details>
<summary>Version notes, and one log line that looks alarming but isn't</summary>

**Take 1.4.8 or newer.** Up to 1.4.7 Chicken rescanned every module in the process every 300
presented frames, looking for a graphics component that might have loaded late. In an application
with a lot of DLLs loaded that scan costs a frame each time it runs, on a fixed cadence of roughly
five seconds at 60 fps. The result is a periodic hitch and poor 1% lows while the average frame
rate stays flat. 1.4.8 replaced the polling with a callback from the operating system, and its log
then says `smart discovery settled: periodic full-module fallback disabled`. If you are stuck on an
older build, the feeder's `stall_log_ms` setting will confirm whether this is what you are seeing.

**`warm-up: re-creating the feature once` is expected.** Chicken arms its hooks a few seconds after
it loads, and it never picks up work that started before it was ready. So the feeder watches for it
to report `ARMED` and rebuilds once at that moment. Without that rebuild you would get plain DLSS
and an idle Chicken. The feeder's own `warmup_rebuild` frame count is not used while Chicken is
present.

**Frame Generation and Ray Reconstruction** pass straight through untouched; Chicken adopts
neither. Its author does not claim Frame Generation works — a live test gave a black screen — and
does not claim 32-bit Vulkan.

`arm=0` in its config is a full off switch that needs a restart. The live `enabled=0` only stops the
neural work, leaving its hooks in place — worth knowing if you are trying to rule Chicken out of a
problem, because only `arm=0` truly takes it out of the picture.

The interop is ABI 1 in 1.4.0-alpha, 1.4.4-alpha and 1.4.8-alpha alike (`FEEDER-INTEROP-v1.md` is
byte-identical across all three). The protocol is in `src/feed_dfc.h`; the request that produced it
is in `FEEDBACK-DFC.md`.

</details>

<details>
<summary>Where it has been confirmed working</summary>

Confirmed on this machine on 2026-09-02:

- **DOOM (2016)**, 64-bit Vulkan, 3840x2160, in-process — ARMED, 1200+ neural frames, zero failures.
- **Fable Anniversary**, 32-bit D3D9 via dgVoodoo2, 3578x2013, through the x64 helper — Chicken
  armed 6 s after the first create, the feeder rebuilt once on its `ARMED` state, and Chicken
  rendered for the rest of the session.
- **A 64-bit D3D11 host**, in-process, 3840x2160 HDR10 — marker accepted (`feeder_marker=1
  legacy_exact=0`), neural frames throughout. This one is a capture/streaming application rather
  than a game, so it also exercised a swapchain that changes size and format live.

D3D12, OpenGL and 32-bit Vulkan are source-contract compatible per its author but **not
game-validated**.

</details>

**Reporting a problem?** Send `dlss5-feed.log`, `deep-fried-chicken.log` and `ReShade.log` from the
same run (plus `host64\dlss5-feed-host.log` for a 32-bit game), and the output of the
[installer's verification output](#install-the-automated-way).

### Alternative: the RenoDX add-on

To use **Krish's `renodx-dlss5.addon64`** (the `#DLSS5` build) instead of Deep Fried Chicken —
[Before you install](#2-you-need-one-neural-add-on-installed-next-to-this-one) has the download and
the list of build generations:

- In step 4, put `renodx-dlss5.addon64` next to the game `.exe` in place of the three Chicken files.
  `nvngx_dlssnr.dll` and `nvngx_dlss.dll` are needed either way. For a 32-bit game it goes into
  `host64\`, again for the same reason Chicken does.
- Its neural rendering is turned on in ReShade's **DLSS 5 Neural Rendering** panel, and on 32-bit
  games mirrored onto our own overlay page — see [Configuration](#configuration).
- The feeder writes `EnableHooks=2`, `NeuralUplift=1` and `NREnableUpscaling=0` when they are unset,
  and unbinds the helper's global hotkeys. Both are RenoDX-specific and do nothing on the Chicken path.
- `warmup_rebuild` matters here: older builds latch STANDBY on their first create, and the warm-up
  re-create is what clears it.

## Install for a 32-bit game (beta)

NGX only exists as 64-bit code, so a 32-bit game gets a 64-bit helper process that does the DLSS
work. Two folders to fill:

**Next to the game `.exe`** (32-bit side)
- ReShade, installed with its installer (it detects 32-bit itself; tick "Enable loading of add-ons").
- `dlss5-feed.addon32` from the [latest release](https://github.com/jlrouzies-fr/DLSS5-Feeder/releases/latest).
- `DLSS5_Feed.fx` into `reshade-shaders\Shaders\`, plus a motion-vector provider (steps 3 and 5 of the
  [64-bit instructions](#install-for-a-64-bit-game)).

**In a new `host64\` folder next to the game `.exe`** (64-bit side, nothing here can go beside the game)
- `dlss5-feed-host64.exe` from the same release.
- A 64-bit ReShade `dxgi.dll` (run the ReShade installer once against any 64-bit game and take it from there).
- The neural consumer: Deep Fried Chicken's three files (`deep-fried-chicken.addon64`,
  `deep-fried-chicken-nvngx.dll`, `deep-fried-chicken.cfg`), or `renodx-dlss5.addon64`
  ([the RenoDX route](#alternative-the-renodx-add-on)).
- `nvngx_dlssnr.dll` and `nvngx_dlss.dll`.

**Then, in-game**
- Turn it on as for a 64-bit game. The first fed frame starts the helper by itself.
- Day-to-day settings: ReShade overlay → Add-ons → **DLSS 5 Feed** (see [Configuration](#configuration)).
- The neural consumer's own panel lives in the helper, not in the game's ReShade. No alt-tab needed:
  press **Show the DLSS 5 panel in-game** on that page, or bind a key with **Set key**, and it is drawn
  at the top right of the game window with your clicks forwarded to it. Changes apply live.
  - That button uses the desktop compositor: windowed or borderless games only.
  - **Show as texture** draws it through the game's own ReShade instead and also works in exclusive
    fullscreen; it appears once the feed has built.
  - While the panel is up the mouse and keyboard belong to it. Escape away from it hides it, Alt+F4
    hides it and closes the game, and the X in its corner always closes it. **Panel size** scales it.
- `host_window=1` in `dlss5-feed.cfg` gives the helper a visible window of its own instead (Home opens
  the panel there).

![32-bit-overlay-ingame](Ingame-32bit-overlay.png)

The helper's own window, with `host_window=1`:

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

A working configuration from issue #15 (World of Warcraft 3.3.5a, DXVK 3.0.2, ReShade 6.8 x86 as
a global Vulkan layer, Guild Wars Reforged likewise): the default `dxvk.conf` is fine; the only
setting that mattered was `dxvk.allowFse = False`. Expect roughly half the frame rate — DLAA at
native size plus the 32→64-bit hop — and, from 0.11.0-beta.2, no more plateaus at exactly 30 fps
(the helper's own `Present` used to block on the compositor while the game waited behind it; it
no longer waits). If frame times still step between two fixed values, run once with `mode=1`
(transport only, no DLSS) and once with `async_home=0` and post both `dlss5-feed.log`s; the
`present probe` line now also counts presents against frames fed here, which tells an external
frame pacer apart from a slow helper.

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

The static test is the one with a memory. Left to decide afresh every frame, it can win on one and
lose the next over a low-contrast surface under a slow pan, so the vector alternates between the
provider's and zero and DLSS alternately reprojects and does not — a flicker that comes and goes and
that no consumer downstream can smooth away. **Static test: require two frames in a row**
(`STATIC_HYSTERESIS`, on by default) only zeroes a vector where the test won twice; on the first win
it keeps the vector and masks the pixel instead. Turn it off to compare against the old behaviour.
Debug view *Validation tests over the image* marks in yellow what was actually zeroed, and in orange
what the hysteresis held back, so a flip is visible rather than inferred.

## How it works

* `DLSS5_Feed.fx` (companion effect) converts the selected provider's motion vectors (delta-UV,
  `prev_uv = uv + mv`) into `DLSS5_MV` (RG16F, **pixels**), copies the raw hardware depth with
  ReShade's orientation fixes into `DLSS5_Depth` (R32F), and flags every vector it does not trust
  in `DLSS5_Mask` (R8). MV, mask and raw depth are emitted by one MRT guide pass, avoiding a second
  full-screen depth pass while leaving the depth values sent to DLSS unchanged.
* `dlss5-feed.addon64` registers with the ReShade add-on API. After the `DLSS5_Feed` technique
  renders, it takes the backbuffer + those textures and runs `NGX_D3D12_EVALUATE_DLSS` in DLAA
  mode (render size = output size, no jitter). The neural consumer
  (`deep-fried-chicken.addon64`, or `renodx-dlss5.addon64`) detours that D3D12 evaluate and inserts
  its neural pass — it cannot tell the contract is synthetic.
* On a **64-bit D3D11 game** the optional **Work resolution** slider can run the private
  DLAA + Neural Rendering contract at 50–100% of each backbuffer axis. Color is resampled
  linearly; depth, motion vectors and the trust mask use point sampling; motion-vector
  magnitudes are corrected for the selected work extent. The result is expanded back over the
  native-size backbuffer, bilinearly or (`work_upscale=1`) through AMD FSR 1 EASU + RCAS. At 100%
  this reduces to the original copy path.
* On a **D3D12 game** there is no transport at all: NGX runs on the game's own device and queue,
  motion vectors and depth are consumed zero-copy straight from the effect textures, and the feature
  survives alt-tabs and effect reloads untouched (only a real resolution change rebuilds).
* NGX calls are wrapped in SEH, a command list the add-on crashed in is discarded rather than
  submitted, and NGX is reinitialized after repeated failures — a faulting closed-source add-on
  disables the feed instead of taking the game down. A failed or crashed feature create is
  retried up to twice, each attempt spaced a full hook-arming grace apart (with an NGX
  re-initialisation before the second), instead of latching the feed off on the spot. On the
  D3D11 bridge the shared textures and the live feature also survive alt-tabs and effect
  reloads now: a runtime recreation re-creates only the feature, and keeps the old one if the
  new create fails.

### The 32-bit path

NGX and the DLSS 5 add-on only exist as x64 code, and a 32-bit process cannot load an x64 DLL — so
`dlss5-feed.addon32` does none of the NGX work itself. Instead:

* It creates the four Color/Output/Depth/MV textures as **cross-process shared** D3D11 resources
  (`D3D11_RESOURCE_MISC_SHARED_NTHANDLE`) on the game's own device, plus two shared fences.
  If the game's device refuses (a feature-level 10.x device cannot bind the UAV the DLSS output
  needs — NFS Most Wanted 2012, issue #33), it switches to the route the GL and Vulkan clients
  use: the host creates the set on D3D12 and hands the handles in, keeps the output's UAV on its
  own side and copies the result into a plain shared texture. The log says so
  (`the host will create the shared set instead`).
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
  ReShade.ini and restarts it to apply them. For live changes it instead *casts* that window into
  the game: a DWM live thumbnail of the host window is registered on the game window (no pixel
  transport, no protocol change), and while the cursor is over it the game's mouse and keys —
  read through ReShade's add-on API and blocked from the game — are posted to the host window as
  ordinary `WM_*` messages, which is where the host's ReShade reads its input.
* Everything that means *waiting on* the host — spawning it, the handshake, and each build request —
  happens on a worker thread, and every pipe transfer has a deadline. The render thread hands a
  build over and returns; the answer is picked up on a later frame. So the expensive part, a host
  starting up (up to 15 s while it loads ReShade, NGX and the ~165 MB model) and every build after
  a resolution or work-resolution change, no longer freezes the game at all. A host that *hangs*
  rather than dies no longer freezes it either: the transfer times out and the add-on treats it as
  a lost host. **Apply** and **Restart** still pause briefly — up to about four seconds — because
  the old helper has to be given time to save its ReShade.ini before the replacement can claim the
  pipe; the reconnect after it is free.
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
| A neural consumer + `nvngx_dlssnr.dll` | **Deep Fried Chicken** (recommended — `deep-fried-chicken.addon64`, `deep-fried-chicken-nvngx.dll`, `deep-fried-chicken.cfg`, from its Discord), or **Krish's `renodx-dlss5.addon64`** `#DLSS5` build as the alternative. Exactly one of them. Not ShortFuse's `renodx-dlss`, which is a different add-on that replaces this project rather than working with it (see [Before you install](#1-you-might-not-need-this-project-at-all)). Neither is included here, and neither bundles `nvngx_dlssnr.dll`. |
| `nvngx_dlss.dll` | a DLSS Super Resolution runtime next to the game (the driver's copy is used otherwise). |
| A motion vector provider | one of five, selected with the `DLSS5_MV_PROVIDER` definition — **[LumeniteFX](https://github.com/umar-afzaal/LumeniteFX) Kernel is recommended** (`=3`); also iMMERSE Launchpad, VORT, LumeniteFX QuantMotion, or anything writing `texMotionVectors` (qUINT, `dh_uber_motion`). **Not DRME — it does not compile on ReShade 6.8.** See [Motion vectors: choosing a provider](#motion-vectors-choosing-a-provider). Install it yourself — nothing third-party is bundled, and our shader includes no third-party files. |
| `dlss5-feed.addon64` (or `.addon32` + `host64\`) + `DLSS5_Feed.fx` | this project. |

## Configuration

The easiest way to change any of this is **ReShade's overlay → Add-ons tab → DLSS 5 Feed**: every
setting below is a live control there (checkboxes, sliders, combos), reading from and saving straight
to `dlss5-feed.cfg`. On 32-bit games **running the RenoDX add-on**, the same page also mirrors that
add-on's neural-rendering settings from the host process (neural uplift, NR intensity/style/local
structure/local tone/auto mask/UI correction) with an **Apply** button — since those live in a
separate process, Apply writes them into `host64\ReShade.ini` and restarts the helper (~2 s without
DLSS; the game keeps rendering, the feed reconnects automatically). Deep Fried Chicken is not
mirrored: its settings live in its own tab in the helper's window and in `deep-fried-chicken.cfg`.

`dlss5-feed.cfg` itself is created automatically next to the add-on and re-read while the game runs,
if you prefer editing the file directly:

| Key | Default | Meaning |
| --- | --- | --- |
| `enabled` | 1 | `0` disables everything, and from 0.12.1-beta.3 it means it: no frames are fed, no effect runtime is queried, no textures are created, and the Vulkan interop hook is not installed. Only the overlay page stays, so the **Enabled** checkbox can undo it. Two consequences worth knowing when using it as an A/B: a Vulkan game needs a **restart** with `enabled=1` (the hook can only be installed before the game's `vkCreateDevice`, which has already happened by the time you tick the box), and on a Vulkan game already running with `enabled=1` the hook stays installed until the game exits even after you untick it. Before beta.3 this switch only stopped the per-frame feed, so "it still crashes with `enabled=0`" did not clear the add-on. |
| `mode` | 2 | 0 inert · 1 transport test (no NGX; on 32-bit it copies only the left half, so a split screen proves the round trip) · 2 full DLSS path. |
| `work_resolution` | 100 | **D3D11 only (64-bit and 32-bit).** 50–100% of each backbuffer axis for the private DLAA + Neural Rendering work textures. The Add-ons overlay slider applies once 400 ms after dragging stops. Other paths remain at 100%. A cost knob, not DLSS upscaling — below 100% the image is downsampled, processed, and expanded back (see the troubleshooting FAQ). |
| `work_upscale` | 0 | **D3D11 only.** How the work-size output is expanded back over the backbuffer: `0` bilinear stretch · `1` AMD FSR 1 (EASU + RCAS), visibly crisper at 50–75% than the stretch · `2` **cfg-only experiment, not recommended:** DLSS Super Resolution on synthetic jitter — measured to cost as much as 100% and to shimmer (see the FAQ); on a 32-bit game the helper creates the SR feature (IPC v6: add-on and helper must be from the same build). Overlay checkbox "FSR 1 expand-back" toggles between 0 and 1. Better filters for `work_resolution`, not DLSS Quality: the result can never exceed the native frame. If the FSR shaders fail to compile the log says so and the spatial path stays bilinear. |
| `work_sharpness` | 0.3 | RCAS strength for `work_upscale` 1 and 2, `0` (off) to `1` (sharpest). At 100% work resolution only the sharpening runs. Overlay slider "Sharpness". |
| `jitter_sign` | 1 | **Diagnostic for `work_upscale=2`, parse-only.** `1` or `-1`: the sign of the grid shift handed to DLSS. On a static scene the right sign converges to a stable image within a second, the wrong one crawls. Here until the convention is confirmed in a game. |
| `jitter_phases` | 0 | **Diagnostic for `work_upscale=2`, parse-only.** Halton sequence length; `0` = NVIDIA's 8 × (native ÷ work)². |
| `hdr` | -1 | -1 auto (FP16 / R11G11B10 backbuffer = HDR), 0 force SDR, 1 force HDR. |
| `depth_inverted` | -1 | -1 follow `RESHADE_DEPTH_INPUT_IS_REVERSED`, 0/1 force. |
| `flags` | -1 | raw `DLSS.Feature.Create.Flags` override. |
| `reset_every` | 0 | 1 = NGX Reset every frame (no temporal history; diagnostic). |
| `warmup_rebuild` | 180 | **RenoDX path only.** Re-create the feature once after N delivered frames, working around older RenoDX builds latching STANDBY on their first create. Skipped automatically on "v45+" builds. **Not used as a frame count while Deep Fried Chicken is present** — there the one re-create is triggered by Chicken's own `ARMED` state instead (it arms its NGX detours seconds after claiming, and never adopts a create it did not see). |
| `rebuild` | 0 | change the number to re-create the feature once, by hand. |
| `log_frames` | 3 | first N frames logged in detail. |
| `create_delay` | 60 | frames to hold a feature (re)build after a runtime (re)init — the neural consumer arms its NGX hooks asynchronously, and calling in too early can crash. 0 disables. |
| `preset` | 0 | DLSS render-preset hint: `0` default, `5`/`6` = legacy CNN presets E/F (clamp history harder — try these if motion warps around transparents like dust or flames), `10`/`11` = transformer presets J/K. |
| `gpu_timeout_ms` | 2000 | how long a frame waits for the GPU to retire a command allocator before that frame is abandoned. Three abandoned frames in a row stop the feed. Raise it on a heavily contended GPU; clamped to 100–60000. |
| `mv_scale_x/y` | 1.0 | extra motion-vector multiplier. |
| `stall_log_ms` | 50 | **Diagnostic.** Log a breakdown for any frame whose present-to-present interval exceeds this, in ms (0 = off). Each `STALL frame` line splits the interval into the time inside the NGX evaluate call — which is where the neural consumer's own work runs — the rest of this add-on's work, and everything outside it, then names which of the three dominated. The `600 frames:` summary also carries the worst frame and a stall count. Use it to tell "the feed is slow" apart from "the neural consumer is slow" apart from "neither, something else in the process stalled". |
| `async_home` | 1 | **32-bit games only.** 1 = pipelined handoff: each frame carries the DLSS output of the frame *before* it, so the game never waits for the helper process inside a frame — this is what lifts the ~35 fps ceiling of the original same-frame contract (issue #15). Costs one frame of latency on the DLSS output, which the temporal history hides. 0 = the original same-frame behaviour. Also on the overlay as "Pipelined handoff". |
| `host_window` | 0 | **32-bit games only.** 0 keeps the helper's window behind the game, off the taskbar, and lets the overlay's "Show the DLSS 5 panel in-game" button cast its tuning panel into the game window; 1 gives the helper its own visible window instead (press Home there). Read when the helper is started. |
| `cast_key` | 0 | **32-bit games only.** Virtual-key code that shows/hides the cast DLSS 5 panel in-game; 0 = none. Set it from the overlay page with "Set key" rather than by hand. |
| `cast_scale` | 100 | **32-bit games only.** Size of the cast panel, 25..300 % of the largest size that fits the game window (above 100 % it may run past the window's edges). Also on the overlay as "Panel size". |
| `cast_mode` | 0 | **32-bit games only.** How the cast panel is drawn: 0 = a desktop-compositor thumbnail of the helper's window (windowed / borderless games, any API); 1 = a shared copy of the helper's frame drawn by the game's ReShade or blitted onto its backbuffer (works in exclusive fullscreen; D3D11, OpenGL and Vulkan). The two overlay buttons set it. |

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
| `host64\ReShade.log` | 32-bit games: the neural consumer's messages (`feature 18 created`, `inline feature 18 evaluation succeeded`). |
| `deep-fried-chicken.log` | next to the Chicken add-on: its own arming, contract and per-pass state. Quote it with `dlss5-feed.log` on any Chicken report. |

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
* **DLSS 5 panel stuck in STANDBY** (RenoDX add-on) — it missed the first create; the built-in
  warm-up re-creates the feature a few seconds in, which normally clears it.
* **Deep Fried Chicken's tab says `DISARMED`, `CONFLICT` or `FAILED`** — `DISARMED` is `arm=0` in
  `deep-fried-chicken.cfg` (restart-only), or the first launch before its `LoadFromDllMain` entry
  took effect; `CONFLICT` means a second neural consumer (`renodx-dlss5.addon64`,
  `renodx-dlss.addon64`, `alexs-toolkit.addon64`) is loaded — remove it and fully restart.
  `dlss5-feed.log` prints the same state.
* **A regular stutter every few seconds, with poor 1% lows but a normal average frame rate** —
  first check the neural consumer's version. Deep Fried Chicken up to 1.4.7 rescanned every module
  in the process every 300 presented frames, which is about every 5 s at 60 fps and costs a frame
  each time in a host with a lot of DLLs loaded. **1.4.8 fixes it**; its log then says
  `smart discovery settled: periodic full-module fallback disabled`. To confirm the cause rather
  than guess, set `stall_log_ms` in `dlss5-feed.cfg` to roughly one and a half frame times (25 for
  60 fps) and read the `STALL frame` lines: they say whether the lost time was inside the NGX
  evaluate call, inside this add-on, or outside both. `arm=0` plus a full restart is the clean A/B,
  because the live `enabled=0` switch leaves the consumer's observation hooks installed and the
  scans running.
* **Neural rendering stops mid-session, no crash** — the overlay's **Status** section now names the
  reason next to `Session: disabled`, and **Re-enable** restarts it. If the log says `the GPU did
  not retire allocator slot N within 2000 ms`, the GPU is not keeping up rather than broken: raise
  `gpu_timeout_ms`. A single slow frame no longer stops the session — three consecutive failures do.
* **Corruption or flicker with Smooth Motion on** — see [Before you install](#3-turn-off-optiscaler-and-smooth-motion-on-vulkan) at the top of
  this README. The overlay says whether Smooth Motion was detected, and `dlss5-feed.log` records the feeding
  thread: `frame fed from thread N, not the usual M` means `Present` is arriving off-thread, and
  `re-entrant frame … dropped` means it arrived twice at once. Both lines are worth quoting on an
  issue. Turn Smooth Motion off for this game's API only, with Profile Inspector.
* **Smooth Motion on, everything reports healthy, nothing looks neural** — two ReShade runtimes
  (`effect runtime … initialised (device …, window class '…'; 2 runtimes in this process)`), and
  before 0.11.0-beta.2 the feeder could be bound to the wrong one. Update; the log then shows
  `binding to effect runtime …: it is the one rendering DLSS5_Feed`.
* **Can I use DLSS Quality / Balanced / Performance instead of DLAA?** No — not as a setting, and
  not as a future one either. Real DLSS upscaling needs the game to render *smaller* than the
  screen and jitter its camera, then hands DLSS that small frame; the feeder only ever sees the
  finished, screen-sized frame ReShade has, so it publishes a 1:1 DLAA contract by construction.
  `work_resolution` (D3D11) is a **cost** knob: the frame is downsampled, DLAA + neural
  rendering run on the smaller image, and the result is expanded back — that is why 50–66% looks
  blurry rather than like DLSS Quality. From 0.12.0 the expand-back can be AMD FSR 1 instead of a
  bilinear stretch (`work_upscale=1`, or the "FSR 1 expand-back" checkbox under the slider): much
  crisper at 50–75%, still bounded by the native frame. Leave the DLSS 5 add-on's
  `NREnableUpscaling` at 0: with a 1:1 contract it cannot engage, and on v4.6 it parks neural
  rendering for the run. For games that ship DLSS already, use the game's own DLSS (or OptiScaler)
  instead of this feeder.
* **What about adding the jitter ourselves? (`work_upscale=2`, D3D11, experimental)** — it
  exists, and it is honest about its limits. The feeder shifts the downsample grid by a Halton
  sub-pixel offset every frame, reports that offset to DLSS, and creates the feature in Super
  Resolution mode (render = work size, target = native), so DLSS itself rebuilds the native frame
  from the jittered history. Three things it cannot change: the game still rendered every native
  pixel, so there is no frame-rate win on the game side; every jittered sample comes from that
  same native frame, so the best DLSS can converge to is the image you already had at 100%; and
  DLSS SR leans on motion vectors far more than DLAA — with the feeder's *estimated* vectors, a
  rejected history means a blurry low-res frame rather than a sharp native one, so expect smear in
  motion. **Measured (Fable Anniversary, 3578x2013, Deep Fried Chicken 1.4.8): it saves nothing.**
  At 50% work resolution DLAA + FSR 1 ran at 57–62 fps and DLSS reconstruction at 44–48 fps, the
  same as 100%. DLSS Super Resolution's cost follows the *output* size, and the neural consumer
  takes DLSS's resolved native output as its input (`neural=3578x2013->3578x2013` in Chicken's
  log), so the neural pass runs on every native pixel again. The image also shimmers. That is why
  it is **not on the overlay**: it stays reachable as `work_upscale=2` in the cfg for anyone who
  wants to repeat the experiment, and nothing more. What people actually want — render the game
  smaller, run DLSS 5 on that, upscale the neural result — would need a neural model trained to
  upscale, which is NVIDIA's to ship. The only route to a real frame-rate win today is the game
  rendering fewer pixels: set a lower resolution in the game and let the GPU scale it to the
  panel (NVIDIA Control Panel → Adjust desktop size and position → GPU scaling); the feeder then
  works 1:1 at that size. See `PLAN-PROXY-SWAPCHAIN.md` for what doing that in-process would take.
* **32-bit game: changing the work resolution froze the PC for a few seconds, then DLSS never
  came back and the helper window stopped responding** — the helper's log shows `feature create
  did not complete`, its `ReShade.log` shows `DXGI_ERROR_DEVICE_HUNG`, and every later rebuild
  fails with `OpenSharedHandle … 0x887A0005`. Before 0.12.0 the helper released the old feature
  and textures without waiting for the GPU, which a neural consumer that keeps a graph alive on
  the feature (Deep Fried Chicken) turned into a GPU hang, and a helper whose D3D12 device was
  removed retried against it forever. From 0.12.0 the rebuild drains the GPU first, and a helper
  whose device was removed says so, exits, and is respawned by the add-on on the next frame.
* **32-bit game: `tex 1 CreateTexture2D failed 0x80070057`, `failure: shared build` forever** —
  the game's D3D11 device is feature level 10.x and cannot bind the UAV the DLSS output needs.
  From 0.11.0-beta.2 the helper creates the shared set instead (the log says `the host will
  create the shared set instead`); on older builds there is no workaround (`mode=1` uses the same
  texture).
* **The game crashed** — `dlss5-feed.log` (or `dlss5-feed-host.log`) ends with
  `### CRASH RECORDED ###`, naming the exception, the module it faulted in and what the feeder was
  doing at the time, and from 0.11.0-beta.2 a `dlss5-feed-crash.dmp` is written next to it. Post
  the log and the `.dmp` on the issue; the last ordinary line before the crash line (the session
  opens in fixed steps — adapter, D3D12 device, NGX init, multithread protection, session open)
  pins the step.

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
| `spike\build-spike.bat` | the standalone proofs used during development: the 32↔64-bit shared-resource pair, plus `spike-gl64.exe` / `spike-gl32.exe` and `spike-vkhost64.exe` / `spike-vkclient32.exe`, which round-trip a texture and a fence between D3D12 and OpenGL / Vulkan, in-process and cross-process, and `spike-proxy-swapchain.exe`, the `IDXGISwapChain` wrapper contract behind `PLAN-PROXY-SWAPCHAIN.md` (a 960×540 "game" presented at window size through FSR 1). They need an NVIDIA GPU to *run*, none to compile. | — |

NGX links against the Release CRT, so the builds use `/MD`.

Each script picks up its toolchain through `toolscvars.bat`, which asks `vswhere` for the latest
Visual Studio install with the C++ tools and falls back to a fixed BuildTools path. Set `VCVARSALL`
to your own `vcvarsall.bat` to override it.

**CI** — `.github/workflows/build.yml` builds all five targets on every pull request, fetching the
NGX SDK and the Vulkan headers from their upstream repositories, and uploads the binaries as a
workflow artifact. It only proves the tree compiles and links: DLSS needs an RTX GPU and a real
swapchain, so nothing in the table under [Status](#status) can be verified there.

## Limitations and roadmap

* **DLAA contract, optional reduced work extent on D3D11** — render resolution still
  equals DLAA output resolution, but the private work extent can be 50–100% of the native
  backbuffer and is spatially expanded afterward (bilinear, or FSR 1 with `work_upscale=1`).
  D3D12, Vulkan and OpenGL paths remain at 100%. This is not jittered DLSS Super Resolution, and a
  Quality/Balanced/Performance mode cannot be added: see the FAQ entry in
  [Logs and troubleshooting](#logs-and-troubleshooting).
* Estimated motion vectors → temporal artifacts in fast motion; the UI is processed with the scene
  (a UI mask / pre-UI colour capture is future work).
* **Geometry vectors are experimental and off.** The camera-model fit is derived from the provider's
  own flow, so it inherits that noise, and it has no way to know the HUD is not part of the scene.
  Doing it properly needs the game's real view-projection matrices, not a fit.
* **Synthetic-jitter DLSS reconstruction (`work_upscale=2`) is a cfg-only experiment, not on
  the overlay.** It turns DLSS into the expand-back for the work-resolution cost knob, and the
  measurement says it costs as much as 100% (DLSS SR follows the output size; the neural consumer
  runs on the resolved native output) and shimmers (the jitter sign convention, `jitter_sign` on
  the 64-bit add-on, was never confirmed). D3D11 only. On the 32-bit path the add-on ships the
  target size with each build and the grid shift with each frame (IPC v6) and the helper creates
  the SR feature; no covering preset means a DLAA + FSR 1 rebuild. Kept so the result can be
  reproduced; use FSR 1.
* A light that flickers faster than DLSS's history converges is averaged into a slow pulse. The
  trust mask reduces it; nothing available to a post-process feed removes it.
* Exclusive-fullscreen swapchain churn can make some games reload effects repeatedly; windowed is
  smoother.
* Depends on a closed-source, community-distributed neural consumer and the NGX runtime; both can
  change. Deep Fried Chicken is game-validated here only on 64-bit Vulkan in-process and through
  the 32-bit x64 helper — its other backends are source-contract compatible per its author, and it
  claims neither Frame Generation nor 32-bit Vulkan.
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
* **DLSS 5 neural rendering:** **Deep Fried Chicken** by Alexander — the recommended consumer, and
  the author who built the ABI-1 interop this feeder negotiates over (`FEEDER-INTEROP-v1.md`) — and
  the RenoDX community's `renodx-dlss5` add-on, the alternative. Neither is bundled here.
* **ReShade** add-on API by Patrick Mours.
* **dgVoodoo2** by Dege — the D3D9 translation layer that makes the DirectX 9 path possible.
* **D3D12 stability findings** independently confirmed by the
  [Pizzawookiee fork](https://github.com/Pizzawookiee/DLSS5-Feeder)'s diagnostics.

## License

MIT — see [LICENSE](LICENSE). This covers only the code in this repository (`src/`, `shaders/`,
`host/`); the dependencies above keep their own licenses.
