# CK3 DLSS Vulkan — Internal Documentation

This directory contains the source code and build scripts for the CK3 DLSS Vulkan package.

## Directory Structure

```
ck3-package/
  binaries/           - Prebuilt binaries included in the package
    dlss-payload/     - DLSS feeder addon and runtimes
    dlss5-vulkan/     - Vulkan layer and ReShade64
    dxgi.dll          - App-local DXGI/D3D12 bootstrap
    ReShade.ini       - ReShade configuration
    DLSS5-CK3.ini     - DLSS5 configuration
    reshade-shaders/  - ReShade shader files
    third-party/      - Third-party license notices
  tools/              - Installer tools
    CK3-DLSS-Installer/ - Installer GUI application
    RHI-Setup.exe     - RHI setup utility
  README-INTERNAL.md  - This file
  THIRD-PARTY-NOTICES.md - Third-party notices
  THIRD-PARTY-LICENSES - License files
```

## Build Outputs

The build process generates:
- `build\dlss5-feed.addon64` - DLSS feeder addon
- `layer\VkLayer_feed_vk.dll` - Vulkan layer
- `layer\dxgi.dll` - DXGI bootstrap

## Key Components

- **DLSS5 Feeder** (`src\dlss5-feed.cpp`) - NGX-based DLSS integration
- **Vulkan Layer** (`layer\feed_vk_layer.cpp`) - Package-local Vulkan interceptor
- **DXGI Bridge** (`layer\dxgi_bridge.cpp`) - App-local D3D12 bootstrap
- **ReShade** - Graphics overlay framework
- **RenoDX/RHI** - DLSS 5 Neural Rendering extension
- **VORT Shaders** - Motion vector estimation shaders
- **Lilium HDR Shaders** - HDR analysis and tone mapping suite

## Profiles

Four runtime profiles are supported:
1. **DLSS 4.5 Model M** - Recommended baseline
2. **DLSS 5 Stock** - Signed DLSS 5 runtime
3. **DLSS 5 Extended** - Experimental compatibility
4. **Native Streamline Vulkan** - Streamline interposer experiment