# Crusader Kings III portable DLSS renderer

This is the source template for a drag-and-drop CK3 package. A built release is extracted over the
**Crusader Kings III** directory that already contains `binaries`, `game`, and `launcher`. It keeps
ReShade and both Vulkan layers local to CK3: no Vulkan registry keys and no global ReShade install.

## Install

1. Extract the release ZIP into the **Crusader Kings III** folder, not directly into `binaries`.
2. Run **`Install CK3 DLSS.cmd`**.
3. Pick a runtime profile. The installer explains any download and asks before acquiring proprietary
   or community runtime files. It verifies x64 PE headers, NVIDIA signatures where applicable, and
   records versions, URLs, signatures, and SHA-256 hashes.
4. The installer backs up CK3's settings and changes only `Graphics.renderer` to `Vulkan`.
5. Start CK3 with **`Launch CK3 with DLSS.cmd`**. Press **Home** in game and confirm Lumenite Kernel
   runs above DLSS 5 Feed.

Use **`Configure CK3 DLSS Runtime.cmd`** later to switch profiles. Running CK3 normally leaves the
portable Vulkan layers inactive. **`Disable CK3 DLSS.cmd`** also changes CK3's saved renderer back
to DX11; it does not delete files.

## Runtime profiles

| Profile | Loaded components | Best use |
|---|---|---|
| **DLSS 4.5 / DLAA** | Feeder + official `nvngx_dlss.dll` | Recommended RTX 20/30/40 baseline, including RTX 3060 |
| **DLSS 5 Neural Rendering** | Above + RenoDX add-on + NVIDIA-signed `nvngx_dlssnr.dll` selected through RHI's manifest | Hardware supported by that preview runtime |
| **DLSS 5 Extended** | Above + unsigned modified ShortFuse NR runtime from RHI | Experimental RTX 20/30/40/50 test path |

The first profile is native-resolution **DLAA**, because the feeder supplies identical input and
output dimensions. It tests DLSS 4.5 reconstruction/anti-aliasing quality but does not provide an
upscaling FPS gain. It defaults to DLSS 4.5 Model M (`preset=13`), a more practical test choice on an
RTX 3060. In ReShade's Add-ons tab you can compare Model K (`11`, DLSS 4), Model L (`12`, heavier
DLSS 4.5 quality), and Model M (`13`, DLSS 4.5 balanced).

The Extended profile is deliberately never selected automatically. RHI describes its ShortFuse
runtime as extending Neural Rendering to older RTX generations, but it is modified, unsigned, and
not an official NVIDIA compatibility promise. Treat it as unstable preview tooling.

Do not enable NVIDIA Smooth Motion, OptiScaler, or a second DLSS/Streamline injector at the same
time. CK3's UI is part of the frame and is processed by the pass. Estimated motion vectors can
ghost during fast map movement because CK3 has no native motion-vector/DLSS integration.

## RHI integration

The one-click installer uses RHI's maintained public runtime manifest and component-release API for
the optional RenoDX/Neural Rendering pieces, then deploys only the selected files into this package.
No full RHI installation is required.

**`Open RHI Runtime Manager.cmd`** can download and open the latest official RHI installer as an
optional GUI companion. RHI has no supported headless component-install CLI. Its Vulkan ReShade
mode is global (`ProgramData` plus Vulkan registry entries) and can conflict with this package's
explicit `VK_LAYER_reshade`; do not let RHI install/manage Vulkan ReShade for CK3 while using the
portable launcher. Keep this package's `ReShade.ini`, Generic Depth, Lumenite, and preset settings
authoritative.

## Portable layout

```text
Crusader Kings III\
  Install CK3 DLSS.cmd
  Configure CK3 DLSS Runtime.cmd
  Launch CK3 with DLSS.cmd
  Open RHI Runtime Manager.cmd
  Disable CK3 DLSS5.cmd
  DLSS5-CK3.ps1
  DLSS-Runtime-Setup.ps1
  binaries\
    ck3.exe                              (already supplied by the game)
    ReShade.ini
    DLSS5-CK3.ini
    dlss-payload\
      dlss5-feed.addon64
      runtimes\...                      (optional pre-bundled inputs)
    dlss-active\                        (created by installer; only this is loaded)
      dlss5-feed.addon64
      dlss5-feed.cfg
      nvngx_dlss.dll
      renodx-dlss5.addon64              (Neural Rendering profiles only)
      nvngx_dlssnr.dll                  (Neural Rendering profiles only)
      CK3-DLSS-RUNTIME.json
    dlss-cache\                         (download cache and per-profile settings)
    reshade-shaders\Shaders\...
    reshade-shaders\Textures\...
    dlss5-vulkan\
      ReShade64.dll
      ReShade64.json
      VkLayer_feed_vk.dll
      VkLayer_feed_vk.json
```

ReShade loads add-ons only from `binaries\dlss-active`. Switching to DLSS 4.5 physically removes
the RenoDX add-on from that active directory, so the RTX 3060 baseline cannot accidentally invoke
Neural Rendering.

## Build a release

Thin package (recommended; no NVIDIA/RenoDX binaries in the ZIP):

```powershell
.\Build-CK3-Package.ps1 `
  -ReShadeSetup C:\Downloads\ReShade_Setup_Addon.exe
```

Optionally make an offline/pre-bundled package by supplying any locally obtained runtime files:

```powershell
.\Build-CK3-Package.ps1 `
  -ReShadeSetup C:\Downloads\ReShade_Setup_Addon.exe `
  -DlssRuntime C:\Downloads\nvngx_dlss.dll `
  -RenoDxAddon C:\Downloads\renodx-dlss5.addon64 `
  -DlssNrRuntime C:\Downloads\nvngx_dlssnr.dll
```

The builder downloads the published DLSS feeder, its Vulkan layer, LumeniteFX, and standard ReShade
headers from their project repositories. It extracts `ReShade64.dll` from the supplied full/add-on
ReShade setup, validates all supplied PE files, and creates `release\CK3-DLSS-Portable.zip`.

Thin packages acquire NVIDIA/RenoDX components on the user's machine only after explicit consent.
For offline runtime setup, the same three paths can be passed to `DLSS5-CK3.ps1`; package builders
can instead pre-stage them with the optional arguments above. Building a ZIP never grants
redistribution rights—confirm every supplied binary's terms before publishing a pre-bundled build.

