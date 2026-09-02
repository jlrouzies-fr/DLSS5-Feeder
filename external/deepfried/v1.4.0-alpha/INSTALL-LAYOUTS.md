# Installation folder diagrams

Use only the diagram matching the game's real rendering path. The folder shown
is the one containing the actual game executable, not a Steam, EA, Rockstar,
or other launcher executable.

`[DFC]` means supplied in this archive. `[USER]` means you must obtain and
verify it yourself. `[OTHER]` belongs to ReShade, the game, or an optional
transport and is not supplied by Chicken.

## Native D3D12 game with DLSS Super Resolution

```text
<folder containing the real 64-bit game.exe>\
|-- game.exe                                      [OTHER]
|-- dxgi.dll                                      [OTHER: ReShade proxy]
|-- ReShade.ini                                   [OTHER]
|-- deep-fried-chicken.addon64                    [DFC]
|-- deep-fried-chicken-nvngx.dll                  [DFC]
|-- deep-fried-chicken.cfg                        [DFC]
`-- nvngx_dlssnr.dll                              [USER: trusted x64 copy]
```

The game may already have `nvngx_dlss.dll` here or load it from the driver.
Do not replace that file for the first test. Add
`deep-fried-chicken.addon64` to `[ADDON] LoadFromDllMain` in `ReShade.ini`, or
use Chicken's **Enable early load** button and restart.

## Native D3D11 game that already supports DLSS

```text
<folder containing the real 64-bit game.exe>\
|-- game.exe                                      [OTHER]
|-- <ReShade proxy created by its installer>      [OTHER]
|-- ReShade.ini                                   [OTHER]
|-- deep-fried-chicken.addon64                    [DFC]
|-- deep-fried-chicken-nvngx.dll                  [DFC]
|-- deep-fried-chicken.cfg                        [DFC]
|-- dlss5-dx11-bridge.addon64                     [DFC]
|-- dlss5-dx11-bridge.cfg                         [created on first run]
`-- nvngx_dlssnr.dll                              [USER: trusted x64 copy]
```

The bridge only translates an existing native D3D11 DLSS contract. It does not
add DLSS to a game that lacks it.

## 64-bit game without native DLSS, using DLSS5-Feeder

```text
<folder containing the real 64-bit game.exe>\
|-- game.exe                                      [OTHER]
|-- <ReShade proxy created by its installer>      [OTHER]
|-- ReShade.ini                                   [OTHER]
|-- deep-fried-chicken.addon64                    [DFC]
|-- deep-fried-chicken-nvngx.dll                  [DFC]
|-- deep-fried-chicken.cfg                        [DFC]
|-- nvngx_dlssnr.dll                              [USER: trusted x64 copy]
|-- dlss5-feed.addon64                            [OTHER: Feeder]
|-- dlss5-feed.cfg                                [OTHER: Feeder]
`-- reshade-shaders\
    `-- Shaders\
        `-- DLSS5_Feed.fx                         [OTHER: Feeder]
```

For this layout:

```text
KEEP:    dlss5-feed.addon64
REMOVE:  renodx-dlss5.addon64 / renodx-dlss.addon64
DO NOT ADD: dlss5-dx11-bridge.addon64
```

Start with `warmup_rebuild=0`, one Chicken pass, 100% Feeder work resolution,
Texture Boost off, Smooth Motion off, and OptiScaler off.

## 32-bit game using Feeder's companion x64 host

The 64-bit Chicken and NVIDIA files must be inside Feeder's `host64` folder.
They cannot load beside the 32-bit game executable.

```text
<folder containing the real 32-bit game.exe>\
|-- game.exe                                      [OTHER: x86]
|-- dlss5-feed.addon32                            [OTHER: matching Feeder]
|-- dlss5-feed.cfg                                [OTHER: matching Feeder]
|-- reshade-shaders\
|   `-- Shaders\
|       `-- DLSS5_Feed.fx                         [OTHER: Feeder]
`-- host64\
    |-- dlss5-feed-host64.exe                     [OTHER: same Feeder build]
    |-- dxgi.dll                                  [OTHER: x64 ReShade]
    |-- ReShade.ini                               [OTHER: host]
    |-- deep-fried-chicken.addon64                [DFC]
    |-- deep-fried-chicken-nvngx.dll              [DFC]
    |-- deep-fried-chicken.cfg                    [DFC]
    |-- nvngx_dlss.dll                            [USER/Feeder requirement]
    `-- nvngx_dlssnr.dll                          [USER: trusted x64 copy]
```

The x86 add-on, x64 host, shaders, layer files, and configuration must all come
from one version-matched Feeder package. Remove any Reno neural provider from
`host64`. If Chicken's tab is absent from the host overlay, add
`deep-fried-chicken.addon64` to the host's `ReShade.ini` early-load list and
restart both processes.

## Files that do not belong in the runtime folder

The Markdown documentation, licence files, and `SHA256SUMS.txt` may remain
where you extracted the archive. They are not loaded by the game. Do not copy
every optional binary blindly: the native-DLSS D3D11 bridge and DLSS5-Feeder
are alternative transports, not components to stack together.
