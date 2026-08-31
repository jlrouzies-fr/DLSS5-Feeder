# CK3 DLSS Vulkan — All Profiles

This is a drag-and-drop Vulkan/ReShade/DLSS package for the Windows version of **Crusader Kings III**.
It includes the locally built DLSS feeder, an app-local DXGI/D3D12 bootstrap, portable Vulkan layers,
ReShade, VORT motion-vector shaders, the complete Lilium HDR Shaders 2026.02.28 suite, RHI, and the files for four runtime profiles.

## Install

1. Close Crusader Kings III and the Paradox launcher.
2. In Steam, right-click **Crusader Kings III**, select **Manage → Browse local files**.
3. Open `CK3-DLSS-Vulkan-All-Profiles.zip` and copy **everything inside the ZIP** directly into the
   `Crusader Kings III` folder.
4. Confirm the layout is correct. `Launch CK3 with DLSS.cmd` must be beside the existing `binaries`,
   `game`, and `launcher` folders. Do not extract the package into `binaries` and do not leave it
   inside an extra `CK3-DLSS-Vulkan-All-Profiles` folder.
5. Run **`Open CK3 DLSS Installer.cmd`**. The app validates the selected CK3 folder, reports the
   active profile, and offers the three supported choices:

   - **DLSS 4.5 Model M** — recommended native-resolution DLAA baseline.
   - **DLSS 5 Stock** — DLSS 5 Neural Rendering with the signed stock runtime.
   - **DLSS 5 Extended** — experimental compatibility profile using the modified ShortFuse runtime.

   The original profile-specific `.cmd` installers remain available as command-line fallbacks.

6. Start the game with **`Launch CK3 with DLSS.cmd`**. This launcher supplies the package-local
   Vulkan-layer environment. Launching CK3 normally through Steam does not activate these layers.
   RHI may remain installed; the launcher disables its global ReShade Vulkan layer only for CK3.

The installer validates the package, creates `binaries\dlss-active`, backs up CK3's renderer setting,
and changes `Graphics.renderer` to `Vulkan`. It installs the package-owned `binaries\dxgi.dll`
bootstrap but does not replace `binaries\ck3.exe`.

## ReShade controls

Press **Home** in game to open ReShade. The focused DLSS preset intentionally contains these effects:

1. `vort_MotionEffects`
2. `DLSS5_Feed`
3. `vort_StaticEffects`

Keep the motion effect above `DLSS5_Feed`. Lilium HDR Shaders 2026.02.28 are bundled for HDR analysis,
tone mapping, inverse tone mapping, black-floor correction, and HDR-aware sharpening.
Open **Add-ons -> DLSS 5 Feed -> Neural reconstruction
model** to switch live among Runtime Default, E, F, J, K, L, and M. Changing the model immediately
rebuilds the NGX feature and saves the selection to `dlss5-feed.cfg`. DLSS 5 profiles also expose
controls for the separate RenoDX/Neural Rendering extension.

## Change profiles

Run **`Install CK3 DLSS Native Streamline Experimental.cmd`** to enable the opt-in NVIDIA
Streamline Vulkan interposer profile. It initializes the DLSS and DLSS-RR plugins before CK3 requests
`vulkan-1.dll`, keeps CK3 on the system Vulkan loader handle, and routes proc-address lookups
through a compatibility shim. Device and swapchain calls continue through Streamline, while Win32
surface lifecycle and capability calls use the system loader to accommodate CK3's device-first startup.
Direct Streamline resource tagging and evaluation are not yet wired, so the existing Vulkan-to-D3D12
NGX feeder remains the evaluation fallback.

Close CK3, then reopen **`Open CK3 DLSS Installer.cmd`** and select another profile. You can also
run a profile-specific command or **`Configure CK3 DLSS Runtime.cmd`**. Each switch rebuilds
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
- **Shader selection** — the package includes its DLSS/VORT effects and the complete Lilium HDR
  suite, not every general-purpose ReShade collection.
- **Artifacts or ghosting** — CK3 has no native DLSS motion-vector integration. VORT estimates motion,
  so fast map movement, UI, smoke, and transparency can ghost.

The default profile is native-resolution DLAA with Model M neural reconstruction, not an upscaling
performance mode. The reconstruction processes the entire frame, including character faces, using
color, depth, and motion-vector inputs.
