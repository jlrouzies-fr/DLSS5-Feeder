# DLSS5 Feeder Architecture

## Overview

This document describes the architectural design of the Crusader Kings III DLSS Vulkan integration package. The system enables DLSS 4.5/5 rendering for CK3 by injecting a package-local Vulkan layer, DXGI bootstrap, and ReShade framework that work together to deliver DLSS-enhanced rendering.

## Core Components

### 1. App-Local DXGI Bridge (`binaries\dxgi.dll`)

The `dxgi.dll` is a custom x64 DLL that serves as the foundation for the entire DLSS pipeline. It is **not** a replacement Vulkan driver and does not translate all CK3 rendering to D3D12.

**Key functions:**

1. **Loads package's private ReShade64.dll** through its DXGI exports
2. **Creates a hidden D3D12 device, command queue, and DXGI swapchain**
3. **Performs an initial `Present`** so ReShade and RenoDX receive the initialization events they expect
4. **Keeps the hidden runtime alive** while the feeder creates its private D3D12 device and DLSS feature
5. **Leaves the existing Vulkan/D3D12 shared-texture and shared-fence transport** responsible for moving CK3's frame to DLSS and returning the processed result to Vulkan

**Why it's required:** CK3's Vulkan renderer does not naturally load a DXGI proxy or create a D3D12 presentation runtime. The RenoDX DLSS 5 add-on hooks D3D12 NGX entry points, and when RHI installs ReShade as `dxgi.dll` for a D3D12 game, ReShade starts early and RenoDX observes the D3D12 device lifecycle before the game creates or evaluates its DLSS feature. Loading ReShade only as a Vulkan layer discovers the add-ons, but by itself does not guarantee that those D3D12 hooks are armed.

### 2. Vulkan Layer (`layer\VkLayer_feed_vk.dll`)

The Vulkan layer is the primary entry point that intercepts CK3's Vulkan calls. Key aspects:

- **Package-local**: Does not register as a global Vulkan layer
- **Exports `DLSS5Bootstrap`**: The dxgi.dll calls this export before Vulkan device creation
- **Compatibility shim**: Keeps the system Vulkan loader handle expected by CK3 and owns `vkGetInstanceProcAddr` plus `vkGetDeviceProcAddr`
- **Routes calls strategically**:
  - Device and swapchain calls continue through Streamline (or the package's interposer)
  - Win32 surface creation, destruction, presentation support, and capability queries use the system Vulkan loader
- **Failure handling**: If initialization fails, CK3 uses the normal Vulkan loader without interception
- **Logging**: Records results in `binaries\dlss5-dxgi.log`

### 3. DLSS Feeder (`build\dlss5-feed.addon64`)

The feeder is the NGX-based DLSS integration component:

- **Built from**: `src\dlss5-feed.cpp` and `src\dlss5-feed32.cpp`
- **Output**: `build\dlss5-feed.addon64`
- **Function**: Feeds color frame, depth, and motion vectors to the NVIDIA DLSS runtime
- **Motion vector estimation**: Uses VORT shaders since CK3 has no native DLSS motion-vector integration
- **Profile support**: Supports multiple DLSS runtime profiles (DLSS 4.5 Model M, DLSS 5 Stock, DLSS 5 Extended)

### 4. ReShade Integration

ReShade provides the graphics overlay framework:

- **Activated via**: Press **Home** in game
- **DLSS preset**: Contains `vort_MotionEffects`, `DLSS5_Feed`, `vort_StaticEffects` (motion effect must be above DLSS5_Feed)
- **Add-ons tab**: DLSS preset selection and RenoDX/Neural Rendering controls appear here
- **Lilium HDR Shaders**: 2026.02.28 suite bundled for HDR analysis, tone mapping, inverse tone mapping, black-floor correction, and HDR-aware sharpening

### 5. RenoDX / RHI Runtime Integration

RenoDX enables DLSS 5's Neural Rendering extension:

- **Hooked through**: The app-local `dxgi.dll` creates a hidden D3D12 device
- **Initialization sequence**: Hidden Present → ReShade/RenoDX initialization → feeder device creation
- **Profile dependencies**:
  - DLSS 5 Stock: Requires RenoDX
  - DLSS 5 Extended: Requires RenoDX with modified ShortFuse runtime
  - DLSS 4.5: Does not require RenoDX

### 6. Profile System

Four runtime profiles are supported, each with different component combinations:

| Profile | Components | Intended use |
|---|---|---|
| **DLSS 4.5 Neural Reconstruction / DLAA** | Local feeder + NVIDIA DLSS runtime | RTX 20/30/40 baseline; Model M neural reconstruction |
| **DLSS 5 Stock** | Feeder + RenoDX + signed DLSS/Neural Rendering pair | Hardware supported by the stock preview runtime |
| **DLSS 5 Extended** | Feeder + RenoDX + modified ShortFuse Neural Rendering runtime | Experimental compatibility testing |
| **Native Streamline Vulkan** | NVIDIA Streamline interposer + DLSS/DLSS-RR plugins + stock NGX fallback | Experimental Vulkan interception and integration testing |

### 7. Installer Architecture

The installer (`Open CK3 DLSS Installer.cmd`) performs these operations:

1. **Validates** the bundled x64 renderer and runtime files
2. **Creates** `binaries\dlss-active` containing only the selected profile's files
3. **Backs up** CK3's current renderer setting
4. **Changes** `Graphics.renderer` to `Vulkan`
5. **Installs** the package-owned `binaries\dxgi.dll` bootstrap
6. **Requires license acknowledgement** before enabling installation

**Profile switching**: Close CK3 and reopen `Open CK3 DLSS Installer.cmd` to change profiles. Each switch rebuilds `binaries\dlss-active` so files from the previous profile are not left loaded.

### 8. Launcher (`Launch CK3 with DLSS.cmd`)

The launcher supplies the package-local Vulkan-layer environment:

- **Required**: Steam's normal Play button does not activate this package
- **Function**: Sets environment variables and launches CK3 with the correct Vulkan layer path
- **RHI suppression**: Disables RHI/global ReShade Vulkan layer only for the CK3 process

### 9. Disable/Restore Mechanism (`Disable CK3 DLSS.cmd`)

Restores CK3 to its pre-installation state:

1. **Restores** the recorded renderer setting
2. **Deactivates** portable Vulkan layers when CK3 is launched normally
3. **Package files remain available** but are inactive

## Technical Flow

```
User launches CK3 via Launch CK3 with DLSS.cmd
    ↓
Launcher sets Vulkan layer environment variables
    ↓
CK3 loads package's VkLayer_feed_vk.dll
    ↓
Vulkan layer calls DLSS5Bootstrap in dxgi.dll
    ↓
dxgi.dll loads ReShade64.dll and creates hidden D3D12 runtime
    ↓
Hidden Present triggers ReShade/RenoDX initialization
    ↓
dxgi.dll creates feeder's private D3D12 device
    ↓
DLSS feeder (dlss5-feed.addon64) processes frame
    ↓
DLSS runtime enhances frame
    ↓
Processed result returned to Vulkan for presentation
```

## Build Outputs

```
build\dlss5-feed.addon64          (DLSS feeder addon)
layer\VkLayer_feed_vk.dll         (Vulkan layer)
layer\dxgi.dll                    (DXGI bootstrap)
```

## Known Constraints

- **No native DLSS motion-vector integration**: CK3 has no native motion vectors; VORT estimates motion, causing potential ghosting during fast map movement, UI elements, smoke, and transparency
- **Experimental status**: This is prototype software with known limitations
- **Profile compatibility**: Do not combine the active package with OptiScaler, Smooth Motion, or a second DLSS/Streamline injector
- **Streamline identity**: The donor Streamline runtime does not have a valid NVIDIA application identity for CK3, so direct NGX features may remain disabled until supported project identity and resource tagging are added