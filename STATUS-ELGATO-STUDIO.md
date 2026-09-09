# Elgato Studio (WinUI 3) — deploy status

**Working as of 2026-09-08.** Feeder + OptiScaler DLSS-NR running in Elgato Studio, overlay usable
with keyboard and mouse. Getting there needed a **custom ReShade build**; stock 6.8.0.1 cannot run
in this app at all.

This file is the status of *this target*. The reusable knowledge is in `deploy/SOURCES.md`
(`reshade-winui3/` section); `DEPLOY-DEV.md` has no WinUI 3 section yet (see Open items).

## Target

| | |
| --- | --- |
| Path | `C:\Program Files\WindowsApps\Elgato.Studio_1.1.0.1714_x64__g54w8ztgkx496` |
| Identity | `Elgato.Studio` 1.1.0.1714, x64 MSIX, `Windows.FullTrustApplication`, publisher Corsair |
| Render | D3D11, WinUI 3 on `Microsoft.WindowsAppRuntime.1.8`, Win2D, WebView2; presents via `CreateSwapChainForComposition` 3840x2160 `R10G10B10A2_UNORM` flip |
| GPU | RTX 5090, driver 616.64 |

## Deployed layout

Everything sits next to `Elgato.Studio.exe` inside the package folder:

```
dxgi.dll                  deploy/reshade-winui3/dxgi_x64.dll   <-- CUSTOM build, not the stock cache
dlss5-feed.addon64        0.14.0-beta.5, rebuilt from source
version.dll               OptiScaler-DLSSNR-v0.2.0 OptiScaler.dll, renamed (was winmm.dll; version.dll
                          is what is deployed and verified — the feeder's "never loaded a DLL of that
                          name" warning does not appear on this name)
OptiScaler.ini            12 keys edited section-aware (below)
nvngx.dll_dlssnr.dll      OptiScaler zip root
OptiScaler\               OptiScaler zip runtime folder (9 files)
nvngx_dlss.dll            deploy/shared/
nvngx_dlssnr.dll          deploy/shared/
ReShade.ini               deploy/templates/ReShade.ini.d3d
ReShadePreset.ini         deploy/templates/ReShadePreset.ini  (DLSS5_MV_PROVIDER=3)
reshade-shaders\          DLSS5_Feed.fx + 3 framework .fxh + 8 lumenite_*.fx + 4 include .fxh + bluenoise
```

No renodx, no Deep Fried Chicken, no Alex's Toolkit, no dx11-bridge — verified absent.

`OptiScaler.ini`: `[DlssNr] Enabled=true`, `AutoCapture=false`; `[Upscalers] Dx12Upscaler=dlss`;
`[Log] LogToFile=true LogLevel=2`; `[Spoofing] Dxgi=false StreamlineSpoofing=false`; `[Inputs]`
the three non-DLSS `Enable*=false`; `[Hotfix] CheckForUpdate=false`; **`[Menu] OverlayMenu=false`**
(see below). `ScanExposure` does not exist as a key in v0.2.0 — `feed_opti.h:178` defaults it to
false, which is what we want.

### `[Menu] OverlayMenu=false` is required here — Insert does nothing without it

OptiScaler's default overlay menu hooks the swapchain, and it only knows
`CreateSwapChainForHwnd` (56 references in `version.dll`) and `CreateSwapChainForCoreWindow` (22).
**`CreateSwapChainForComposition` appears zero times**, in `version.dll` and in every DLL under
`OptiScaler\`. WinUI 3 presents only through that call, so OptiScaler never gets a swapchain, never
initialises its overlay, and the Insert key has nothing to toggle.

`OverlayMenu=false` switches it to the older path that draws inside the upscaler's own output during
Evaluate — which the feeder does drive — so the menu appears and Insert toggles it
(`OptiInput::ApplyMenuVisibilityChangeLocked menu visibility changed 0 -> 1`, and back on the second
press). This is an OptiScaler limitation, unrelated to the ReShade patch.

Watch for `OptiInput::LogInputHealthSnapshotLocked menu is visible but no window/queue/raw input was
received this frame` in `OptiScaler.log`: OptiScaler's own input path has the same WinUI 3 problem
that had to be patched in ReShade (no output window, `WM_POINTER` on a separate input thread), so
mouse interaction inside *its* menu may be unreliable even though the keyboard toggle works. The
`[DlssNr]` knobs are all settable in the ini as a fallback, applied on restart.

`Dx12Upscaler` is the right key even though the app is D3D11: the feeder only calls
`NVSDK_NGX_D3D12_*` (29 sites in `src/dlss5-feed.cpp`) and bridges D3D11 textures over shared handles.

## The custom ReShade — `deploy/reshade-winui3/`

`dxgi_x64.dll`, sha256 starts `49c828cb8947b1cc`, byte-identical to what is deployed. Built from
crosire/reshade tag `v6.8.0` (commit `18deaa5`) + `reshade-6.8.0-winui3.patch` (330 added lines over
`res/exports.def`, `source/d2d1/d2d1.cpp`, `source/d3d11/d3d11.cpp`, `source/input.hpp`,
`source/input_windows.cpp`, `source/runtime.cpp`).

Five problems, each found by bisection or by logging rather than guesswork:

1. **Crash at start** — `dwmcorei.dll`, `0x88990003` (`D2DERR_UNSUPPORTED_OPERATION`), fault offset
   `0x724d3`. The lifted compositor hands its D3D11 device to D2D through a private path ReShade's
   D2D1 hooks never see, and D2D refuses a proxied device. Fix: devices requested by
   `dwmcorei.dll`/`dcompi.dll` are created **unproxied**. The requester is found by walking the stack —
   `_ReturnAddress()` alone reports `WINMM.dll`, because OptiScaler's own hook sits in between.
2. **`d3d11.dll` layout dead end** — "Entry Point Not Found":
   `Elgato.MFComponents.dll`/Win2D import `CreateDirect3D11SurfaceFromDXGISurface` /
   `...DeviceFromDXGIDevice` from d3d11.dll and ReShade exports neither. Added as pass-throughs.
   Not needed for the shipped `dxgi.dll` layout, but keeps that option open.
3. **No overlay** — a composition swap chain has no output window, so `get_hwnd()` returned null and
   the runtime bound no input at all. Fix: fall back to the process main window. Then Home still did
   nothing, because WinUI 3 focuses a *grandchild* (`InputSiteWindowClass` under
   `Microsoft.UI.Content.DesktopChildSiteBridge`) and ReShade walked only one parent level — now the
   whole parent chain.
4. **Mouse frozen** — WinUI 3 delivers mouse input as `WM_POINTER*` (outside
   `WM_MOUSEFIRST..WM_MOUSELAST`) on its own input thread, to a message-only window that is nobody's
   child. Fix: accept pointer messages, reroute them like raw input, and poll the cursor each frame
   (through the `GetCursorPos` **trampoline** — the hooked one returns the app's stale position while
   the overlay blocks input, which caused a snap-to-centre).
5. **Clicks ignored, then pointer stuck centred** — the pointer copies WinUI posts carry only
   `POINTER_MESSAGE_FLAG_NEW` in `wParam` (observed: flags `0x1` on every down *and* up), so button
   bits are useless; button state is read from `GetAsyncKeyState` (trampoline) instead. And while the
   overlay has the mouse the pointer runs in **relative mode** (deltas + warp the real cursor to the
   window centre), because the surface is displayed scaled — absolute mapping left the overlay's
   top-left under the title bar, where WinUI treats input as non-client and sends no click. Final
   bug: `handle_window_message` reset `_mouse_position` from every routed message, pinning the
   pointer to the warped cursor — polling is now authoritative when active.

Behaviour for ordinary games is unchanged: the compositor bypass keys off two module names, and
the input changes only engage when a swap chain has no output window.

Optional `ReShade.ini` knob, if the overlay pointer should map to the preview panel exactly rather
than the whole client area (only affects the non-relative path):

```
[INPUT]
CompositionSurfaceRect=left,top,width,height
```

### Rebuilding it

Clone `--recursive --branch v6.8.0`, apply the patch, then from a shell with a **pip-capable Python
first on PATH** (glad generates its loader at build time):

```
MSBuild ReShade.sln /t:ReShade /p:Configuration=Release /p:Platform=64-bit /p:PlatformToolset=v145
```

Build the **.sln**, not the .vcxproj (`stb.props` keys off `$(SolutionDir)`), and note the solution
platform is literally named `64-bit`, not `x64`.

## Verified in the running app

| check | evidence |
| --- | --- |
| launch | window `Elgato Studio`, no `Application Error` events |
| ReShade | attached to the app's composition swap chain, 3840x2160 R10G10B10A2; all 9 effects compiled |
| feeder | `feature ready: 3840x2160 DLAA`, 60 fps, feed GPU ~8.5 ms/frame, MV probe 91-100% non-zero |
| OptiScaler | `working as version.dll`, `_CreateFeature result: NVSDK_NGX_Result_Success`; feeder: `NGX calls are routed through OptiScaler DLSS-NR (VERSION.dll)`, fingerprint `MinHWArchitecture 0, MinOSVersion 10.0.10240.16384` |
| neural pass | `DlssNr_Dx12::Dispatch DLSS-NR running at 3840x2160, guides 3840x2160`; `nvngx_dlssnr.dll` + `nvngx.dll_dlssnr.dll` loaded |
| ReShade overlay | opens on Home; mouse moves and clicks land (user-confirmed) |
| OptiScaler menu | opens on Insert with `[Menu] OverlayMenu=false` (user-confirmed); toggle seen in `OptiScaler.log` |

Depth is flat (`min 0, max 0`) — expected, a capture app has no depth buffer; the feeder says so itself.

## Gotchas for next time

- **Never judge a launch by PID.** A modal "Entry Point Not Found" keeps the process alive; that is
  how the d3d11 layout first looked like a success. Check `MainWindowTitle` **and** the
  `Application Error` event log.
- **An Elgato Studio update wipes this.** MSIX installs into a new version-stamped folder, so the
  whole set has to be copied in again. Everything needed is cached — a copy, not a rebuild.
- Launch the packaged app with `explorer.exe 'shell:appsFolder\Elgato.Studio_g54w8ztgkx496!App'`.
- The package folder is writable here and carries no MSIX integrity enforcement; only files were
  added, nothing of Corsair's was modified.

## Open items

- **Feeder false negative, under the `winmm.dll` name only.** While OptiScaler was deployed as
  `winmm.dll`, `dlss5-feed.log` warned *"winmm.dll is an OptiScaler build, but this game never loaded
  a DLL of that name"* even though OptiScaler was loaded and the neural pass ran — the `feed_opti.h`
  check runs at attach, ~1.5 s before Media Foundation pulls winmm in. Under `version.dll` the
  warning does not appear at all and detection is clean, which is why version.dll is the deployed
  name. Worth re-checking presence after the first evaluate rather than only at attach.
- **`DEPLOY-DEV.md` has no WinUI 3 section** (would be §9c, pointing at the custom build).
- **Diagnostics still in the ReShade patch** — the per-message `mouse-range message` /
  `pointer message` / `WM_KEYDOWN` logging was left in deliberately; strip it if the patch is ever
  offered upstream.
- **Upstream-worthy.** Items 1, 3 and 4 are general WinUI 3 bugs, not Elgato-specific — any
  WindowsAppRuntime app hits them.
