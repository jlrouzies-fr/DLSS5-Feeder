# NVIDIA Streamline SDK Headers

This repository contains vendor-edited integration headers from **NVIDIA-RTX/Streamline 2.12.0**.
The actual runtime DLL remains a separately supplied, NVIDIA-signed component that must be obtained
from NVIDIA and placed in your project's library path.

## Overview

Streamline is NVIDIA's performance analysis and optimization SDK for real-time graphics applications.
It provides a C API for integrating with Direct3D 11/12 and Vulkan, enabling features like:

- Performance monitoring and profiling
- DLSS (Deep Learning Super Sampling) support
- Reflex latency measurement
- NIS (NVIDIA Image Scaling)
- Security and device management

## Header Files

### Core Headers (always include these first)

| Header | Purpose |
|--------|---------|
| `sl.h` | Main entry point — includes all other headers and defines feature import macros |
| `sl_version.h` | Version constants (`SL_VERSION_MAJOR=2`, `SL_VERSION_MINOR=12`, `SL_VERSION_PATCH=0`) and `Version` struct with `toStr()`/`toWStr()` methods |
| `sl_result.h` | `SL_CHECK`, `SL_FAILED`, `SL_SUCCEEDED` macros and the `Result` enum with error codes (eOk, eErrorIO, eErrorDriverOutOfDate, etc.) |
| `sl_consts.h` | `SL_ENUM_OPERATORS_64` macro for bitwise AND/OR operations on 64-bit enums |

### Engine & Device Setup

| Header | Purpose |
|--------|---------|
| `sl_appidentity.h` | `EngineType` enum (`eCustom`, `eUnreal`, `eUnity`, `eCount`) for engine identification |
| `sl_core_api.h` | Core API function declarations (`slInit`, `slSetD3DDevice`, `slSetVulkanInfo`, etc.) |
| `sl_core_types.h` | Typedefs for D3D11/D3D12 resources and Vulkan types (`SL_VKResult`, `HRESULT`, `CommandBuffer`) |

### Feature-Specific Headers

| Header | Purpose |
|--------|---------|
| `sl_helpers.h` | General helpers — includes Reflex, PCL, DLSS, and NIS feature includes |
| `sl_helpers_vk.h` | Vulkan-specific helper functions (e.g., `getVkPhysicalDeviceVulkan12Features`) |
| `sl_dlss.h`, `sl_dlss_d.h`, `sl_dlss_g.h` | DLSS features (base, direct, and game-specific variants) |
| `sl_nis.h` | NIS (NVIDIA Image Scaling) support |
| `sl_reflex.h` | Reflex latency measurement framework |
| `sl_pcl.h` | PerfKit/Performance Capture Library support |
| `sl_nvperf.h` | **NSight Perf SDK** — profiling hooks (licensed separately, see LICENSE.txt) |
| `sl_security.h` | Security functions using Windows Crypto API for license/authentication verification |

### Utility Headers

| Header | Purpose |
|--------|---------|
| `sl_hooks.h` | Hooking utilities for integrating with graphics APIs |
| `sl_matrix_helpers.h` | Matrix transformation utilities |
| `sl_deepdvc.h` | Deep learning super sampling / upscaling utilities |
| `sl_directsr.h` | DirectStorage support |
| `sl_result.h` | Result type and error checking macros |
| `sl_struct.h` | Core data structure definitions |
| `sl_template.h` | Template utilities |
| `sl_helpers_vk.h` | Vulkan-specific helpers |
| `sl_version.h` | Version information |
| `sl.h` | Main header (see above) |

## Usage Example (C++)

```cpp
#include "sl.h"
#include <iostream>

int main() {
    // Initialize Streamline
    sl::Preferences prefs;
    auto result = slInit(prefs, sl::Version(SL_VERSION_MAJOR, SL_VERSION_MINOR, SL_VERSION_PATCH));
    
    if (result != sl::Result::eOk) {
        std::cerr << "Failed to initialize Streamline: " << result << std::endl;
        return 1;
    }
    
    // Set up your graphics device
    // slSetD3D11Device(device) or slSetVulkanInfo(vk_info)
    
    // Use features as needed...
    // e.g., slEnableReflex(), slEnableDLSS(), etc.
    
    return 0;
}
```

## Building

1. Obtain the NVIDIA Streamline runtime DLL (`sl.dll` / `sl_nvperf.dll`) from NVIDIA
2. Place it in your project's library/path directory
3. Link against the appropriate import library
4. Include the `include/` directory in your compiler's include path
5. The `SL_API` macro handles `extern "C"` decoration automatically based on `SL_INTERPOSER` definition

## License

- **Core files**: MIT-like license (see LICENSE.txt) — free to use, modify, and distribute
- **`sl_nvperf.h` and `sl_nvperf.dll`**: Covered by the NSight Perf SDK License (see LICENSE.txt) — separate licensing required

For full licensing details, see LICENSE.txt.
