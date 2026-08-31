# CK3 DLSS Vulkan — All Profiles

This is a drag-and-drop Vulkan/ReShade/DLSS package for the Windows version of **Crusader Kings III**.
It includes the locally built DLSS feeder, portable Vulkan layers, ReShade, VORT motion-vector shaders,
RHI, and the files for all three runtime profiles.

## Install

1. Close Crusader Kings III and the Paradox launcher.
2. In Steam, right-click **Crusader Kings III**, select **Manage → Browse local files**.
3. Open `CK3-DLSS-Vulkan-All-Profiles.zip` and copy **everything inside the ZIP** directly into the
   `Crusader Kings III` folder.
4. Confirm the layout is correct. `Launch CK3 with DLSS.cmd` must be beside the existing `binaries`,
   `game`, and `launcher` folders. Do not extract the package into `binaries` and do not leave it
   inside an extra `CK3-DLSS-Vulkan-All-Profiles` folder.
5. Run exactly one profile installer:

   - **`Install CK3 DLSS 4.5 RTX 3060 Test.cmd`** — recommended RTX 3060 baseline; DLAA with
     DLSS 4.5 Model M neural reconstruction (`preset=13`) across the entire frame, including character
     faces. RenoDX and the separate DLSS 5 Neural Rendering extension are not loaded.
   - **`Install CK3 DLSS 5 Stock Test.cmd`** — DLSS 5 Neural Rendering with the signed stock runtime.
   - **`Install CK3 DLSS 5 Extended Test.cmd`** — experimental compatibility profile using the
     modified ShortFuse runtime.
   - **`Install CK3 DLSS.cmd`** — automatic profile selection.

6. Start the game with **`Launch CK3 with DLSS.cmd`**. This launcher supplies the package-local
   Vulkan-layer environment. Launching CK3 normally through Steam does not activate these layers.

The installer validates the package, creates `binaries\dlss-active`, backs up CK3's renderer setting,
and changes `Graphics.renderer` to `Vulkan`. It does not replace `binaries\ck3.exe`.

## ReShade controls

Press **Home** in game to open ReShade. The focused DLSS preset intentionally contains these effects:

1. `vort_MotionEffects`
2. `DLSS5_Feed`
3. `vort_StaticEffects`

Keep the motion effect above `DLSS5_Feed`. Generic color, bloom, CRT, and sharpening shader packs are
not required for DLSS and are not included. Open **Add-ons -> DLSS 5 Feed -> Neural reconstruction
model** to switch live among Runtime Default, E, F, J, K, L, and M. Changing the model immediately
rebuilds the NGX feature and saves the selection to `dlss5-feed.cfg`. DLSS 5 profiles also expose
controls for the separate RenoDX/Neural Rendering extension.

## Change profiles

Close CK3, then run another profile installer listed above. You can also run
**`Configure CK3 DLSS Runtime.cmd`** for the interactive selector. Each switch rebuilds
`binaries\dlss-active` so files from the previous profile are not left loaded.

## Disable or restore CK3

Run **`Disable CK3 DLSS.cmd`** to restore the renderer recorded before installation. The package files
remain available, but its portable Vulkan layers are inactive when CK3 is launched normally.

## Troubleshooting

- **`binaries\ck3.exe was not found`** — the ZIP was extracted into the wrong folder or one directory
  too deep. Move the ZIP's contents into the CK3 folder that already contains `binaries\ck3.exe`.
- **Validation failed** — close CK3, extract the ZIP again with overwrite enabled, and rerun the chosen
  profile installer.
- **No ReShade overlay** — use `Launch CK3 with DLSS.cmd`; Steam's normal Play button does not set the
  portable Vulkan-layer variables.
- **Few ReShade effects** — expected. This package installs the effects used by the DLSS path, not the
  optional general-purpose ReShade shader collection.
- **Artifacts or ghosting** — CK3 has no native DLSS motion-vector integration. VORT estimates motion,
  so fast map movement, UI, smoke, and transparency can ghost.

The default profile is native-resolution DLAA with Model M neural reconstruction, not an upscaling
performance mode. The reconstruction processes the entire frame, including character faces, using
color, depth, and motion-vector inputs.
