# Crusader Kings III DLSS Vulkan

Drag-and-drop Vulkan/ReShade/DLSS integration for the Windows version of **Crusader Kings III**.

The package changes CK3 to Vulkan, loads ReShade and the DLSS feeder through package-local Vulkan
layers, and provides three selectable runtime profiles. It does not replace `ck3.exe`, install a
global Vulkan layer, or affect other games.

## Download

Download **`CK3-DLSS-Vulkan-All-Profiles.zip`** from the latest GitHub release.

## Install

1. Close Crusader Kings III and the Paradox launcher.
2. In Steam, right-click **Crusader Kings III**, then select **Manage → Browse local files**.
3. Copy everything inside `CK3-DLSS-Vulkan-All-Profiles.zip` directly into that folder.
4. Confirm `Launch CK3 with DLSS.cmd` is beside the existing `binaries`, `game`, and `launcher`
   folders. Do not extract the package directly into `binaries` or leave it inside an extra folder.
5. Run one profile installer:

   - **`Install CK3 DLSS 4.5 RTX 3060 Test.cmd`** — recommended RTX 3060 baseline.
   - **`Install CK3 DLSS 5 Stock Test.cmd`** — signed DLSS 5 Neural Rendering runtime.
   - **`Install CK3 DLSS 5 Extended Test.cmd`** — experimental compatibility runtime.
   - **`Install CK3 DLSS.cmd`** — automatic profile selection.

6. Start CK3 with **`Launch CK3 with DLSS.cmd`**.

The included launcher is required because it supplies the local Vulkan-layer environment. Steam's
normal Play button does not activate this package.

## Runtime profiles

| Profile | Components | Intended use |
|---|---|---|
| **DLSS 4.5 Neural Reconstruction / DLAA** | Local feeder + NVIDIA DLSS runtime | RTX 20/30/40 baseline; second-generation transformer Model M (`preset=13`) by default |
| **DLSS 5 Stock** | Feeder + RenoDX + signed DLSS/Neural Rendering pair | Hardware supported by the stock preview runtime |
| **DLSS 5 Extended** | Feeder + RenoDX + modified ShortFuse Neural Rendering runtime | Experimental compatibility testing |

DLSS 4.5 uses native-resolution DLAA with Model M neural reconstruction across the entire frame,
including character faces. It consumes the current color frame, depth, and motion vectors to reconstruct
a temporally stable native-resolution result; it is not an upscaling performance mode. This is distinct
from the separate DLSS 5 Neural Rendering extension. The Extended profile is experimental and may be unstable.

To change profiles, close CK3 and run another profile installer. You can also use
**`Configure CK3 DLSS Runtime.cmd`**.

## ReShade

Press **Home** in game to open ReShade. The focused preset contains:

1. `vort_MotionEffects`
2. `DLSS5_Feed`
3. `vort_StaticEffects`

Keep `vort_MotionEffects` above `DLSS5_Feed`. Generic color-grading, bloom, CRT, and sharpening
shader packs are unrelated to DLSS and are not included.

Feeder controls, including DLSS preset selection, appear in ReShade's **Add-ons** tab. DLSS 5
profiles also expose RenoDX/Neural Rendering controls.

## What the installer changes

- Validates the bundled x64 renderer and runtime files.
- Creates `binaries\dlss-active` containing only the selected profile.
- Backs up CK3's current renderer setting.
- Changes `Graphics.renderer` to `Vulkan`.
- Keeps ReShade and both Vulkan layers local to CK3.

Run **`Disable CK3 DLSS.cmd`** to restore the renderer recorded before installation. The package's
Vulkan layers are inactive when CK3 is launched normally.

## Package layout

```text
Crusader Kings III\
  Install CK3 DLSS.cmd
  Install CK3 DLSS 4.5 RTX 3060 Test.cmd
  Install CK3 DLSS 5 Stock Test.cmd
  Install CK3 DLSS 5 Extended Test.cmd
  Configure CK3 DLSS Runtime.cmd
  Launch CK3 with DLSS.cmd
  Disable CK3 DLSS.cmd
  DLSS5-CK3.ps1
  DLSS-Runtime-Setup.ps1
  Graphics-Dependency-Setup.ps1
  tools\
    RHI-Setup.exe
  binaries\
    ck3.exe                              (provided by CK3)
    ReShade.ini
    DLSS5-CK3.ini
    dlss5-vulkan\
      ReShade64.dll
      ReShade64.json
      VkLayer_feed_vk.dll
      VkLayer_feed_vk.json
    dlss-payload\
      dlss5-feed.addon64
      runtimes\DLSS45\...
      runtimes\DLSS5\...
      runtimes\DLSS5Extended\...
    dlss-active\                         (created by the installer)
    reshade-shaders\Shaders\
      DLSS5_Feed.fx
    third-party\vort_Shaders\
```

## Build from this repository

The release feeder and Vulkan layer are compiled from the current working tree. Building does not
require pulling or replacing the checkout.

Requirements:

- Visual Studio 2022 Build Tools with the MSVC x64 toolchain and Windows SDK.
- NVIDIA NGX SDK headers and `nvsdk_ngx_d.lib` under `external\ngx`.
- Khronos Vulkan headers under `external\vulkan`.

Build the local feeder and layer:

```cmd
build-local-current-tree.cmd
layer\build-layer-local.cmd
```

Outputs:

```text
build\dlss5-feed.addon64
layer\VkLayer_feed_vk.dll
```

CK3 packaging scripts and the drag-and-drop template are under [`ck3-package`](ck3-package).

## Known limitations

- This is experimental prototype software.
- CK3 has no native DLSS motion-vector integration. VORT estimates motion vectors, so fast map
  movement, UI elements, smoke, and transparency can ghost.
- The CK3 interface is part of the processed frame.
- Do not combine this package with another ReShade installation, OptiScaler, Smooth Motion, or a
  second DLSS/Streamline injector.

## Troubleshooting

- **`binaries\ck3.exe was not found`** — extract the ZIP into the CK3 root, one directory higher.
- **Validation failed** — close CK3, extract the ZIP again with overwrite enabled, and rerun the
  selected profile installer.
- **No ReShade overlay** — launch with `Launch CK3 with DLSS.cmd`, not Steam's normal Play button.
- **Few ReShade effects** — expected; only the effects used by the DLSS path are included.
- **Need to recover** — run `Disable CK3 DLSS.cmd`, then launch CK3 normally.

## Included projects

- DLSS5 Feeder
- ReShade
- VORT shaders
- RenoDX/RHI runtime integration
- NVIDIA DLSS runtimes

See [`THIRD-PARTY-NOTICES.md`](ck3-package/THIRD-PARTY-NOTICES.md) and
[`THIRD-PARTY-LICENSES`](ck3-package/THIRD-PARTY-LICENSES) for bundled-component notices.
