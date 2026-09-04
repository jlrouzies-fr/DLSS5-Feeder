# Deploying DLSS5-Feeder into a game — fast path for an AI agent

This is a deployment runbook, not user documentation — see `README.md` for that. It exists
so a future agent doesn't have to re-derive the install from scratch (and re-hit the ACL /
per-effect-preprocessor gotchas below) every time.

## 0. Figure out the target first

- **Bitness**: `file <game>.exe` (or check Task Manager once it's running) → 32-bit or 64-bit.
- **Render API**: 64-bit D3D11/D3D12 → local `dxgi.dll`. Vulkan → machine-wide layer, no
  local ReShade DLL. OpenGL → local `opengl32.dll` (ReShade's OpenGL install), and nothing
  else: no layer, no hook, no registry. D3D9 → needs dgVoodoo2 first (see README "Install
  for a DirectX 9 game"), out of scope for automated deploy.
  - A **32-bit** game showing Vulkan is almost certainly running
    [DXVK](https://github.com/doitsujin/dxvk) — check for `dxgi.dll`/`d3d9.dll`/`d3d11.dll`
    next to the exe that are DXVK's, not the system's. That matters twice: ReShade must go
    in as a **Vulkan layer** (those DLL names are taken), and the deploy is the 32-bit one,
    `host64\` subfolder and all. Untested in a real game as of 0.8.0-beta.1 — see
    `PLAN-VULKAN32.md`.
- **GPU**: DLSS/NGX needs an RTX card. Confirm with
  `Get-CimInstance Win32_VideoController | Select Name` before doing anything else.

## 1. Do you have a `deploy/` cache?

`deploy/` (gitignored, local to this checkout) holds everything a deploy needs, pre-built /
pre-fetched. Check `ls deploy` first.

### If `deploy/` exists

Read `deploy/SOURCES.md` first — it records what's cached, its hash, and how stale it is.
If the target repo's `src/dlss5-feed.cpp` `FEED_VERSION` (or git HEAD) has moved on since
`deploy/addon64/dlss5-feed.addon64` was built, **rebuild just the addon** (step 2 below,
first bullet) and refresh `deploy/addon64/` + `deploy/addon32/` — everything else in
`deploy/` (third-party binaries, LumeniteFX, framework headers) doesn't track this repo's
version and is fine to reuse as-is.

Then skip to step 3 (copy into the game) using files straight from `deploy/`.

### If `deploy/` doesn't exist (or is missing pieces)

Do step 2, then populate `deploy/` the same way it was built originally so the *next*
deploy is instant:

```
deploy/
  addon64/dlss5-feed.addon64          # build.bat output
  addon32/dlss5-feed.addon32          # build-addon32.bat output
  host64/dlss5-feed-host64.exe        # host\build-host.bat output
  vk-layer/VkLayer_feed_vk.{dll,json}, run-with-feed-layer.bat   # layer\build-layer.bat output
  shared/DLSS5_Feed.fx                # shaders\DLSS5_Feed.fx
  shared/renodx-dlss5.addon64         # third-party, see step 2
  shared/nvngx_dlss.dll               # third-party, see step 2
  shared/nvngx_dlssnr.dll             # third-party, see step 2
  reshade-framework/ReShade.fxh, ReShadeUI.fxh, DrawText.fxh   # crosire/reshade-shaders base headers
  motion-vectors/lumenitefx/**        # github.com/umar-afzaal/LumeniteFX (branch: mainline, not main)
  reshade-dxgi/dxgi_x86.dll, dxgi_x64.dll   # ReShade add-on builds, local-install form (D3D path only)
  dgvoodoo2/**                        # D3D9->D3D11 translation, for the DX9 path (32-bit UE3-era games)
  templates/ReShade.ini.vulkan, ReShade.ini.d3d, ReShadePreset.ini
```

## 2. Build / obtain each piece

**This repo's own binaries** — always rebuild from current source, don't trust a cache
older than the working tree:

```
build.bat                 # -> build\dlss5-feed.addon64   (64-bit in-game/D3D/Vulkan add-on)
build-addon32.bat         # -> build\dlss5-feed.addon32   (32-bit in-game stub)
host\build-host.bat       # -> host\dlss5-feed-host64.exe (64-bit helper for the 32-bit path)
layer\build-layer.bat     # -> layer\VkLayer_feed_vk.dll  (out-of-process Vulkan fallback)
```
All four are plain `cl.exe` invocations via `tools\vcvars.bat` — a "`vswhere.exe` is not
recognized" line on stdout is harmless (it falls back fine); only trust the run if you see
`... built.` / a fresh `.addon64`/`.exe`/`.dll` with a newer timestamp than `src/*.cpp`.

**Third-party binaries this repo does NOT redistribute** (`renodx-dlss5.addon64`,
`nvngx_dlssnr.dll`, `nvngx_dlss.dll`) — three ways to get them, in order of preference:

1. Reuse a known-good copy already on this machine. As of 2026-08-31 these folders all
   carry byte-identical copies of the same build:
   `G:\Games\Dusk\`, `G:\Games\Resonance - A Plague Tale Legacy\` (its
   `renodx-dlss5.addon64`, not the older `renodx-dlss5-v2.5.addon64`), and
   `E:\SteamLibrary\steamapps\common\DOOM\`. `deploy/SOURCES.md` has the hashes — diff
   against those before trusting a new copy.
2. The RHI installer: https://github.com/RankFTW/RHI/releases (`gh release view` may say
   "release not found" even though the releases page has assets — check in a browser or
   `gh api repos/RankFTW/RHI/releases` instead of `gh release view latest`).
3. The RenoDX Discord (see README).

`nvngx_dlss.dll` alone (no NR) also ships inside any DLSS-enabled game, or via
[DLSS Swapper](https://github.com/beeradmoore/dlss-swapper).

**Motion-vector provider** — default to **LumeniteFX Kernel** (`DLSS5_MV_PROVIDER=3`, the
README's recommendation):

```
curl -sL https://codeload.github.com/umar-afzaal/LumeniteFX/zip/refs/heads/mainline -o lumenitefx.zip
```

(Note the branch is `mainline`, not `main` — `main` 404s.) Unzip; you need everything in
its `Shaders\` (`lumenite_*.fx` + `include\*.fxh`) and `Textures\lumenite_bluenoise256.png`.
Other providers are `#0` (whatever writes shared `texMotionVectors`), `#1` iMMERSE
Launchpad, `#2` VORT, `#4` LumeniteFX QuantMotion — see README "Motion vectors: choosing a
provider" for what each needs.

**ReShade framework headers** (`ReShade.fxh`, `ReShadeUI.fxh`, `DrawText.fxh`) — every
`.fx` file `#include`s these. Pull from any existing `reshade-shaders\Shaders\` on this
machine (e.g. `G:\Games\Dusk\reshade-shaders\Shaders\`), or from
https://github.com/crosire/reshade-shaders.

## 3. Copy into the game folder

Same file set regardless of bitness/API — only where ReShade itself lives differs (see
step 4). Into the target folder (next to the game's real `.exe`):

```
dlss5-feed.addon64  (or .addon32 for a 32-bit game — see README's 32-bit path for the
                      host64\ subfolder, which needs its OWN full copy of everything: a
                      64-bit dxgi.dll, renodx-dlss5.addon64, nvngx_dlssnr.dll, nvngx_dlss.dll)
renodx-dlss5.addon64
nvngx_dlssnr.dll
nvngx_dlss.dll
reshade-shaders\Shaders\DLSS5_Feed.fx
reshade-shaders\Shaders\ReShade.fxh, ReShadeUI.fxh, DrawText.fxh
reshade-shaders\Shaders\lumenite_*.fx
reshade-shaders\Shaders\include\*.fxh
reshade-shaders\Textures\lumenite_bluenoise256.png
```

Then drop `deploy/templates/ReShadePreset.ini` in as `ReShadePreset.ini`, and either
`ReShade.ini.vulkan` or `ReShade.ini.d3d` as `ReShade.ini` (merge into an existing one if
the game/emulator already ships its own ReShade.ini — keep its other sections, only add/
overwrite `[ADDON]`/`[GENERAL]`).

Nothing else is required. `alexs-toolkit.addon64` in `deploy/` is optional and off the
default path — see section 8 if you want the multi-pass DLSS 5 cascade.

## 4. Where ReShade itself comes from

- **D3D11/D3D12 (64-bit or 32-bit)**: ReShade is a *local* `dxgi.dll` next to the game
  .exe (matching bitness). Get it from https://reshade.me — installer, pick the exe, API
  Direct3D 10/11/12, tick "Enable loading of add-ons" (the unsigned/full build). No global
  registration needed.
- **Vulkan**: ReShade is a **machine-wide implicit layer**, not a local file. Check
  `HKLM:\SOFTWARE\Khronos\Vulkan\ImplicitLayers` / `HKCU:\...` for a `ReShade64.json`
  entry — if `C:\ProgramData\ReShade\ReShade64.{dll,json}` exists, it's already installed
  globally for every Vulkan app and you don't need to run the installer again.
  **But it's gated per-app**: `C:\ProgramData\ReShade\ReShadeApps.ini` has a single
  `Apps=` comma list of full exe paths — ReShade only activates for exes on that list.
  Append the new exe's full path to that line.
  **Gotcha**: that file's ACL is `BUILTIN\Users: Read+Execute` only — `SYSTEM`/
  `Administrators` have write. A non-elevated agent **cannot** edit it; hand the user this
  one-liner to run from an elevated PowerShell:
  ```powershell
  $p = 'C:\ProgramData\ReShade\ReShadeApps.ini'
  $c = (Get-Content -Raw $p).TrimEnd() + ',<FULL PATH TO EXE>'
  [IO.File]::WriteAllText($p, $c, (New-Object Text.UTF8Encoding $true))
  ```
  (Back up the file first — `Copy-Item $p "$p.bak"`.)
  **Also check for a stale entry**: if the game was ever moved between Steam libraries
  (drive letter changed), an old path for it may already be sitting in `Apps=` pointing at
  a folder that no longer has the exe — harmless to leave, but replace it with the current
  path rather than just appending a duplicate.
  **32-bit Vulkan (DXVK)** needs the 32-bit ReShade Vulkan install, i.e. a
  `ReShade32.{dll,json}` implicit layer, and the same `ReShadeApps.ini` gating. Everything
  else is the ordinary 32-bit deploy (`dlss5-feed.addon32` + a full `host64\`). If
  `dlss5-feed.log` then says the Vulkan interop entry points are missing, the fallback is
  `layer\x86\run-with-feed-layer32.bat` — the 32-bit sibling of the layer below, in its own
  subdirectory because the Vulkan loader tries every manifest on `VK_LAYER_PATH` and would
  otherwise pick the 64-bit DLL and skip the layer.
- **OpenGL (64-bit or 32-bit)**: ReShade is a *local* `opengl32.dll` next to the game .exe
  (matching bitness) — installer, pick the exe, API **OpenGL**, tick "Enable loading of
  add-ons". Nothing global, nothing to register, and no `AddonPath` needed: add-ons load
  from ReShade's own directory, which is the game folder. Use `ReShade.ini.d3d` as the
  template (its `[ADDON]`/`[GENERAL]` sections are API-agnostic).
  **No separate download needed**: ReShade ships ONE DLL that serves every API and the
  installer just renames it, so `deploy/reshade-dxgi/dxgi_x86.dll` copied in as
  `opengl32.dll` is exactly right (verified 2026-08-31: 494 exports, all 24 `wgl*` present,
  same export shape as a real ReShade OpenGL install).
  **Check the ReShade version before anything else** — this add-on needs `RESHADE_API_VERSION
  20`, i.e. ReShade **6.8+**. A pre-existing 6.7.x install (API 14) silently refuses to load
  the add-on. `(Get-Item opengl32.dll).VersionInfo.FileVersion` settles it in one command;
  the ReShade version is also the first line of `ReShade.log`.
  **Check the GPU before blaming the add-on**: on a hybrid laptop the game may be rendering
  on the iGPU, where the interop extensions do not exist. `dlss5-feed.log` prints
  `GL_RENDERER` on the session-open line, which settles it in one look.

## 5. The preprocessor-definition gotcha

`DLSS5_MV_PROVIDER` (which motion-vector provider `DLSS5_Feed.fx` reads) is **not** a
global ReShade setting. Despite `README.md` saying "ReShade overlay → select
`DLSS5_Feed.fx` → Preprocessor definitions", ReShade stores that as a **per-effect**
key — inside `ReShadePreset.ini`'s `[DLSS5_Feed.fx]` section, as
`PreprocessorDefinitions=DLSS5_MV_PROVIDER=3`. Setting `[GENERAL] PreprocessorDefinitions=`
in `ReShade.ini` does nothing for this. `deploy/templates/ReShadePreset.ini` already has it
right — this was confirmed by diffing against a live, working DOOM (2016) Vulkan
install's `ReShadePreset.ini` on 2026-08-31, not by guessing.

## 5b. The `d3dcompiler_47.dll` trap (check this before believing any "NR does nothing" report)

If a game folder contains its own `d3dcompiler_47.dll`, Windows loads **that** one instead of
System32's — it is not a KnownDLL, so the application directory wins the search order. A
Windows 8.1-SDK-era copy (file version `6.3.9600.*`, shipped by plenty of games — Space
Engineers among them) knows nothing past Shader Model 5.0, and the DLSS 5 add-on compiles its
neural pass as `cs_5_1`:

```
DLSS5 Generic proxy encode compilation failed with HRESULT 0x8876086c:
error X3506: unrecognized compiler target 'cs_5_1'
```

That pass then fails **every frame, silently**: our own blit shaders are `vs_5_0`/`ps_5_0` and
compile fine, `dlss5-feed.log` reports frames delivered, and neural rendering does nothing.
**Fix: delete or rename the game-folder copy.** For the 32-bit split path the copy that matters
is the one next to `host64\dlss5-feed-host64.exe`, because renodx lives in the host process.

Since 0.11.0-beta.1 both the add-on and the host probe this at startup with a live `cs_5_1`
compile and say so in the log (and the add-on shows a red overlay line). The check is a compile,
not a path test — a game-folder copy may equally be a *newer* one, which is fine (`G:\Games\Dusk`
has a `10.0.26100` copy and is unaffected). One-liner to audit a folder:

```powershell
Get-ChildItem <game folder> -Recurse -Filter d3dcompiler_47.dll |
  ForEach-Object { "{0}  {1}" -f $_.VersionInfo.FileVersion, $_.FullName }
```

## 6. Enable the techniques

`deploy/templates/ReShadePreset.ini`'s `Techniques=` line already pre-enables
`Lumenite_Kernel@lumenite_Kernel.fx` and `DLSS5_Feed@DLSS5_Feed.fx` so the game should come
up with both on. Still verify on first launch (this part genuinely needs a human/running
game, don't claim it's done without checking):

1. Launch the game once.
2. Press **Home** for the ReShade overlay. Confirm no compile errors, and that
   `LUMENITE: Kernel 2.0` + `DLSS 5 Feed` show enabled and `DLSS 5 Feed` is *below*
   `LUMENITE: Kernel 2.0` in the technique order.
3. Enable neural rendering in the **DLSS 5 Neural Rendering** add-on panel (this toggle is
   the add-on's own runtime state, not something this deploy can pre-set).
4. Check `dlss5-feed.log` next to the exe for `feature ready … DLAA`,
   `frame N delivered`, and `DLSS5_MV_PROVIDER=3 (LumeniteFX Kernel) -> Lumenite_Kernel
  (enabled)`. Red text in the overlay's **Motion vectors** section means something's
   still wrong — see README "Are the vectors actually arriving?".

If the log says the Vulkan interop entry points are missing (game creates its device in a
way the `vkCreateDevice` hook doesn't reach), fall back to
`deploy/vk-layer/run-with-feed-layer.bat "<path to game.exe>"` — see `layer/README.md`.

## 7. The 32-bit D3D9 path (dgVoodoo2)

Most pre-~2012 games (Unreal Engine 3-era: Fable Anniversary, Alice: Madness Returns,
Castlevania: Lords of Shadow, …) are 32-bit and D3D9-only. `README.md`'s "Confirm you need
this first" check (look for `Redirecting Direct3DCreate9` in `ReShade.log`) needs the game
running once, which an agent usually can't do — a `PE32 executable ... Intel i386` with an
old linker subsystem version (`MS Windows 5.00`) is a strong enough proxy to deploy for,
just flag that it's unverified.

Full file set (all cached in `deploy/dgvoodoo2/` + `deploy/reshade-dxgi/`, sourced from a
proven-working Fable Anniversary install — see `deploy/SOURCES.md`), copied into the
folder holding the game's real `.exe`:

```
3Dfx\, Cpl\, MS\, D3D9.dll, dgVoodoo.conf, dgVoodooCpl.exe   # dgVoodoo2 itself
dxgi.dll                              # deploy/reshade-dxgi/dxgi_x86.dll, renamed
dlss5-feed.addon32
reshade-shaders\...                   # same set as step 3, minus renodx/nvngx (those go in host64\)
ReShade.ini, ReShadePreset.ini        # same template as the D3D/Vulkan path
host64\dlss5-feed-host64.exe
host64\dxgi.dll                       # deploy/reshade-dxgi/dxgi_x64.dll, renamed — NOT the same
                                       # build as the game-root one; keep them distinct, both cached
host64\renodx-dlss5.addon64, nvngx_dlssnr.dll, nvngx_dlss.dll
host64\ReShade.ini                    # minimal: EffectSearchPaths=.\, TextureSearchPaths=.\,
                                       # NO [ADDON] AddonPath= key (host64 loads its own folder's
                                       # add-ons regardless) — see a working example in
                                       # `Fable Anniversary\...\host64\ReShade.ini`
```

`dgVoodoo.conf` as cached already has the required values (`DisableAndPassThru=false`,
`VideoCard=internal3D`, `VRAM=1GB`, `OutputAPI=d3d11_fl11_0`, `dgVoodooWatermark=true`) —
copy verbatim, no per-game edits needed.

`[DEPTH] DepthCopyBeforeClears=1` in `ReShade.ini` (not `0`, unlike the D3D11/Vulkan
template) matched Fable Anniversary's Unreal Engine 3 depth-clear behavior — reuse `1` for
any other UE3-era game, `0` otherwise.

**This absolutely needs a human to verify** — an agent can't launch the game. Tell the user
to launch it once and confirm the dgVoodoo watermark appears (proves dgVoodoo is active at
all — if not: wrong folder, wrong architecture, or the game turned out not to be D3D9), then
follow step 6 above for the DLSS5-Feeder side. Also point them at `host64\`'s "32-bit DLSS 5
Feeder" window (Home key in it) — the DLSS 5 add-on's full panel lives there, not in the
game's own overlay.

**Host window size** (since 2026-09-04): the host window defaults to tall and narrow — full
work-area height, 620 px wide — so the cast panel sits as a slim column down the right edge
of the game rather than a wide overlay. Override it in `host64\ReShade.ini`'s `[DLSS5Host]`
section (`WindowWidth` / `WindowHeight`, pixels; `WindowHeight=0` means auto/full height —
both keys are written with their in-use values on first run, so they're there to edit even
if you never read this). This is a real resize of the window/swapchain/panel texture, so
ReShade's own UI gets more layout room, not just a bigger-drawn copy of the same pixels —
unlike the cast panel's own in-game "Panel size (%)" slider, which only scales the picture.
`WindowHeight` can exceed the monitor's height on purpose: the window is normally hidden
behind the game, never composited on screen at OS size.

## 8. Optional: Alex's Toolkit (multi-pass DLSS 5 cascade)

`deploy/alexs-toolkit.addon64` is a **third-party, optional** ReShade add-on that makes
DLSS 5 run two or three times per frame. It is not part of a default deploy and nothing
here needs it. Provenance and the full mechanism are in `deploy/SOURCES.md`.

To use it, copy `alexs-toolkit.addon64` **and** `alexs-toolkit.cfg` into the folder where
`renodx-dlss5.addon64` lives — the game folder for a 64-bit game, `host64\` for the
32-bit split-process path. Settings (it re-reads the file live, no restart needed):

```
enabled=1
two_pass=1     # 1 = B->A cascade (two passes). 0 = off, single pass.
three_pass=0   # 1 = B->C->A cascade (three passes). Requires two_pass=1.
```

**Verify it actually armed.** It writes `alexs-toolkit.log` next to itself. Look for
`complete signed resolver set captured; cascade interception is now armed` followed by
`create #1 feature 18: A=... B=... C=...`. If instead you see
`Generic already cached ... before toolkit attach`, it lost the load-order race and stayed
pass-through for the whole run — the cascade did nothing that session.

`dlss5-feed.log` / `dlss5-feed-host.log` now report it too, e.g.
`Alex's Toolkit 0.9.0-beta: 2-pass DLSS 5 cascade active -- roughly 2x the temporal
history`, and the ReShade overlay shows the same line.

**Do not combine it with Deep Fried Chicken** (section 9) — that is a third interposer on the
same NGX module, and Chicken's own test doc asks for it to be removed.

**Expect a temporal cost.** Every stage keeps its own history, so the cascade multiplies
the effective history length. Motion vectors are *not* the problem — they stay
dimensionally valid for every stage — but because ours are screen-space estimates that
are already one frame late, the doubled history shows up as smearing behind fast motion
and a slow settle after a hard camera cut. If a cutscene transition looks like the feeder
briefly stopped working and then recovered, that is the cascade's history re-converging,
not a failure: check `alexs-toolkit.log` for `fallback=` staying at 0 to confirm. Set
`two_pass=0` if the trade isn't worth it for that game.

## 9. Deep Fried Chicken instead of the RenoDX add-on

Deep Fried Chicken (Alexander, 1.4.0-alpha+) is an *alternative neural consumer* — it does
renodx-dlss5's job, attaching to the feature-1 calls this feeder makes. From 1.4.0 it
negotiates rather than collides: `src/feed_dfc.h` publishes the four `DFC.Feeder.*` marker
parameters on every Create and Evaluate, and reads back Chicken's two ABI exports. The
producer contract is `external/deepfried/FEEDER-INTEROP-v1.md`; the request that produced
it is `FEEDBACK-DFC.md`.

Deploy exactly as sections 3-6, with these differences:

```
deep-fried-chicken.addon64        # instead of renodx-dlss5.addon64
deep-fried-chicken-nvngx.dll      # its private NGX bridge (3 KB, not a copy of NVIDIA's)
deep-fried-chicken.cfg            # ships with arm=1, one pass, Texture Boost off = the
                                  # recommended first-test config; no edits needed
nvngx_dlssnr.dll, nvngx_dlss.dll  # still required, same as always
```

**Exactly one neural provider.** Retire `renodx-dlss5.addon64` **and**
`alexs-toolkit.addon64` (section 8) — Chicken stays inert for the whole process if a Reno
provider is loaded, and the toolkit is a third interposer on the same module. Also never add
`dlss5-dx11-bridge.addon64`. The feeder warns about the Reno collision in its log and in red
in the overlay. `warmup_rebuild` is not used as a frame count while Chicken is present: the
feeder polls Chicken's exported `DFC_Feature1InterceptionState` every frame and re-creates the
feature once when it flips to `ARMED`.

**Why that re-create is load-bearing (learned the hard way, Fable Anniversary, 2026-09-02):**
Chicken logs `standalone provider armed: ownership acquired; interop_state=CLAIMING` within a
few hundred ms of loading, but its actual NGX detours land seconds later
(`smart native-NGX Detours commit installed`, 6.4 s later in that run). A feature created in
between is *never* adopted — Chicken's docs say "continuously adopts later creates", and it
does, but only creates it *sees*; there is no adoption at Evaluate. The first build of this
integration switched the warm-up re-create off when Chicken was present (its docs call the
re-create unnecessary), so the one create landed at +1.9 s, Chicken armed at +6.4 s, and the
game ran 5,400 frames of plain DLAA with Chicken idle (`active pairs=0`) while every feeder
log line looked healthy. The `--test` rig never caught it because that mode hard-codes a
second create at frame 180. The tell in `deep-fried-chicken.log` is a `Detours commit` line
with **no** `standalone native DLSS create #1` after it.

For the 32-bit path, the three Chicken files go in `host64\`, not beside the x86 exe.

### Verifying without a game

`host\dlss5-feed-host64.exe --test --hide` is the fastest real check: it loads ReShade, makes
a D3D12 device, and runs 300 evaluates on a 640x360 synthetic contract, so Chicken loads and
runs as a genuine add-on. Build a scratch folder holding the host exe, a 64-bit ReShade
`dxgi.dll` (`deploy/reshade-dxgi/dxgi_x64.dll`), the three Chicken files, both `nvngx_*.dll`,
and a minimal `ReShade.ini` (`[GENERAL] EffectSearchPaths=.\`), then run it there.

What a good run looks like — `dlss5-feed-host.log` / `dlss5-feed.log`:

```
Deep Fried Chicken 1.4.0-alpha: interop ABI 1 (this host speaks ABI 1), ... state 1 (CLAIMING)
Deep Fried Chicken 1.4.0-alpha: ARMED -- consuming the synthetic contract
--test finished: 300/300 evaluates succeeded
```

and `deep-fried-chicken.log`:

```
compatibility transport detected: dlss5-feed-host64.exe; x86-to-x64 host mode is allowed
standalone provider armed: ownership acquired; interop_state=CLAIMING
standalone native DLSS create #1: ... feeder_marker=1 legacy_exact=0
standalone feature 18 created: ... game=640x360->640x360 neural=640x360->640x360
```

**`feeder_marker=1 legacy_exact=0` is the line that matters** — it means Chicken accepted the
negotiated ABI-1 tuple and did *not* fall back to recognising us by filename. `feeder_marker=0`
means our marker never arrived; `legacy_exact=1` means it matched us the old way, so the
marker is wrong or missing.

Known and expected: `DLSS5-Feeder trust mask not published` (the host has never published the
bias-current-colour mask — their docs say so too), and `standalone early load is not
configured`, which is harmless for the feeder path because we create the first feature well
after ReShade starts.

### Status

All verified on 2026-09-02, against Chicken **1.4.0-alpha and 1.4.4-alpha** (same ABI 1;
`FEEDER-INTEROP-v1.md` is byte-identical between them, and the state enum is unchanged):

| Path | Where | Evidence |
| --- | --- | --- |
| In-process (`HostMode=0`) | DOOM (2016), Vulkan, 3840x2160 | ARMED, `feeder_marker=1 legacy_exact=0`, 1200+ neural frames, 0 failures, one clean fenced feature-18 release/recreate |
| Host (`HostMode=1`) | Fable Anniversary in-game (32-bit D3D9 via dgVoodoo2, 3578x2013), plus the `--test` rig | ARMED 6 s after the first create, feeder re-created on the ARMED flip, `feeder_marker=1 legacy_exact=0`, neural frames for the rest of the session |

The 64-bit path publishes the bias-current-colour trust mask and Chicken accepts it
(`trust mask accepted`); the 32-bit host does not publish one, which matches the author's
docs. Chicken's own docs still claim only *source-contract* compatibility for the remaining
backends, and explicitly do not claim Frame Generation (a live test there gave a black
screen) or 32-bit Vulkan.

**Take 1.4.8 or newer.** Up to 1.4.7 Chicken rescanned every module in the process every 300
presented frames looking for a late-loaded NGX host. In a host with a large module list that costs
a frame on a fixed ~5 s cadence: a periodic stutter and poor 1% lows with a flat average frame
rate. Confirmed here on a 64-bit D3D11 capture host and fixed by 1.4.8, which swaps the polling
for an `LdrRegisterDllNotification` callback and logs `smart discovery settled: periodic
full-module fallback disabled`. The feeder's own `stall_log_ms` (see README's config table) is the
tool for attributing a stall like this: it times the NGX evaluate call separately from the rest of
the frame, so "the consumer's detour is slow" is distinguishable from "the feed is slow".

**1.4.4 differences that matter to a deploy:** the Full/Half work-scale selector became a
continuous 10-150% slider (`neural_work_percent` replaces `neural_work_divisor`, old configs
migrate), and early load is now automatic — on its first armed run Chicken appends itself to
`[ADDON] LoadFromDllMain` in the sibling `ReShade.ini`, backs the original up beside it, and
asks for one more full restart. Expect that extra restart on the first launch after installing
it, and expect a `ReShade.ini.deep-fried-chicken-backup-*` file to appear next to it.

## 10. Cutting a release

Tag on the release branch (`v0.12.1-beta.1` and `-beta.2` are both tagged on `v0.12.1`, not
on `main` — `gh release create --target <branch>`).

**Every release carries the automatic-installation notice, in both places.** Most people
land on the release page, not the README, and the manual sections are long enough that
someone will follow them without ever learning the installer exists:

1. `AUTOMATIC_INSTALLATION_AVAILABLE.txt` (repo root) uploaded as a **standalone asset**, so
   it is visible in the assets list before anyone downloads the zip.
2. The same thing, condensed, as a **blockquote at the very top of the release description**,
   above the changelog.

Keep the `.txt` in step with the README's "Install: the automated way" section — it restates
the switches (`-Consumer`, `-LocalFiles`, `-NoElevate`, `-Api`, …), so a change there is a
change here.

The zip asset is the same 12-entry layout every time; build all four binaries first (step 2),
never just the add-on:

```
READ-ME-FIRST.txt                      # what changed in THIS build, plain text, no markdown
Verify-DLSS5Feeder.ps1                 # tools\
dlss5-feed.addon64                     # build\
dlss5-feed.addon32                     # build\
host64/dlss5-feed-host64.exe           # host\
layer-x64/VkLayer_feed_vk.{dll,json}, run-with-feed-layer.bat        # layer\
layer-x86/VkLayer_feed_vk32.{dll,json}, run-with-feed-layer32.bat    # layer\x86\
reshade-shaders/Shaders/DLSS5_Feed.fx  # shaders\
```

Bump `FEED_VERSION` in `src/dlss5-feed.cpp` before building — it is what `dlss5-feed.log`
line 1 prints, and the release notes tell people to check it.
