# Diagnosing issue #47 on your own machine

**The problem.** On some machines `NVSDK_NGX_D3D12_Init` returns `0xBAD00001`
(`FeatureNotSupported`) when DLSS5-Feeder calls it *inside the game process*, while the **same
files on the same driver** initialise NGX successfully inside `host64\dlss5-feed-host64.exe`.
The log then says:

```
[feed] NVSDK_NGX_D3D12_Init -> 0xBAD00001 (FeatureNotSupported)
stopped: NGX would not initialise on the game's device.
```

**Why this document exists.** It does not reproduce on the maintainer's hardware, and a dozen
reports across half a dozen GPUs and three driver versions have not separated the variables —
same driver, opposite results; same architecture, opposite results. Remote guessing has run out
of road. You have the machine that reproduces it; this gives you the instrument and tells you
what each outcome means, so you can settle it locally in one or two runs instead of a week of
round trips.

If you use an AI coding agent (Claude Code, Cursor, Copilot Workspace, …), there is a
ready-to-paste prompt at the bottom. Everything here works fine by hand too.

---

## 1. Run the matrix

Set one environment variable and launch the game normally:

```
set DLSS5_FEED_NGX_MATRIX=1
```

or, from PowerShell:

```powershell
$env:DLSS5_FEED_NGX_MATRIX = '1'
```

Set it in the **same shell you launch the game from**, or through Steam's launch options
(`DLSS5_FEED_NGX_MATRIX=1 %command%` does not work on Windows Steam — use a shortcut or a small
`.bat` that sets the variable and then starts the game).

At session open, `dlss5-feed.log` gains a block like this:

```
[feed] ===== NGX matrix (#47): adapter x DRED x feature level, on throwaway devices =====
[feed] matrix: adapter=the game's own adapter (...) DRED=on  FL=11_0 -> device OK, NVSDK_NGX_D3D12_Init 0xBAD00001 (FeatureNotSupported)
[feed] matrix: adapter=the game's own adapter (...) DRED=on  FL=12_0 -> device OK, NVSDK_NGX_D3D12_Init 0xBAD00001 (FeatureNotSupported)
[feed] matrix: adapter=the game's own adapter (...) DRED=off FL=11_0 -> device OK, NVSDK_NGX_D3D12_Init 0xBAD00001 (FeatureNotSupported)
[feed] matrix: adapter=null = DXGI's default (...)  DRED=on  FL=11_0 -> device OK, NVSDK_NGX_D3D12_Init 0x00000001 (Success)
...
[feed] ===== NGX matrix done. =====
```

It creates throwaway devices, tries each combination, releases them, and then opens the session
exactly as it always would. It changes nothing about how the game runs.

## 2. Read it

Find the rows that say `Init 0x00000001 (Success)`. Then:

| What the matrix shows | What it means | What to do |
|---|---|---|
| **Only `adapter=null` rows succeed** | The adapter argument is the variable. The D3D11 opener passes the *game's* adapter; Vulkan, OpenGL and `host64` all pass null (DXGI's default), which is why those paths work on your machine. | Report it with the block. The fix is a one-line change in `InitSession` (`src/dlss5-feed.cpp`) to pass `nullptr` instead of `adapter`, with a fallback. Say whether your machine has more than one GPU. |
| **Only `DRED=off` rows succeed** | Arming DRED before device creation is what NGX objects to. `host64` never arms it, which would explain the whole split. | Report it. The fix is to make DRED arming conditional, or to arm it after the NGX init rather than before. |
| **Only `FL=12_0` rows succeed** | The feature level is the variable — everything here asks for `11_0` unconditionally. | Report it; this one is a two-line fix. |
| **Every row fails** | It is none of these three. The remaining difference between this process and `host64` is *the process*: what else is loaded into the game. | Go to section 3. |
| **Every row succeeds** | The matrix runs before anything else touches NGX. Something later in the session is breaking it — most likely another add-on's NGX hooks arming in between. | Go to section 3, and attach the full log, not just the matrix block. |

The **data path is not a variable** here, and you can ignore it: `SafeNgxInit12` already tries
the add-on's folder, `host64\`, and `%LOCALAPPDATA%\DLSS5-Feeder\ngx\`, and logs which one was
accepted. That was checked first and is not the cause.

## 3. If the matrix does not separate them

The remaining hypothesis is that something else in the game process has already claimed NGX.
Three things to collect, in order of usefulness:

1. **The NGX runtime identity.** Your log has a line like
   `NGX runtime nvngx_dlssnr.dll: 310.8.2.0 (stated FileVersion "310.8.SF.0")`. There are at
   least three builds in circulation — NVIDIA's, and two repacks. Post that line verbatim.
2. **What else is hooking NGX.** Deep Fried Chicken, OptiScaler and RenoDX all detour
   `nvngx.dll` exports. Move each one out of the folder, one at a time, and re-run. If NGX
   initialises with one of them absent, that is the answer and it is a load-order problem, not a
   driver one.
3. **The bare control.** Run `host64\dlss5-feed-host64.exe --test --hide` from a shell in that
   folder. It creates a device and initialises NGX with nothing else in the process. If that
   succeeds while the in-game matrix fails on every row, the difference is definitively the
   contents of the game process and not your hardware, driver or files.

## 4. What to post

On [issue #47](https://github.com/jlrouzies-fr/DLSS5-Feeder/issues/47):

- the whole `===== NGX matrix =====` block,
- the `[feed] NGX init:` line (it names the transport, the adapter argument, and whether DRED
  was armed),
- the `NGX runtime nvngx_dlssnr.dll:` line,
- GPU, driver version, game, and whether the game is 32-bit or 64-bit,
- which of the outcomes in section 2 you landed on.

A row-by-row result from one machine that reproduces this is worth more than any amount of
further speculation, including the maintainer's.

---

## Prompt for an AI coding agent

Paste this into an agent running in a clone of this repository, on the machine that reproduces
the failure:

> I am debugging issue #47 in DLSS5-Feeder: `NVSDK_NGX_D3D12_Init` returns `0xBAD00001`
> (`FeatureNotSupported`) when the add-on calls it inside the game process, while the same files
> on the same driver initialise NGX successfully inside `host64\dlss5-feed-host64.exe`.
>
> Read `DIAGNOSE-47.md` first, then:
>
> 1. Have me launch the game with `DLSS5_FEED_NGX_MATRIX=1` set, and read the
>    `===== NGX matrix =====` block out of `dlss5-feed.log`.
> 2. Work out which of the three variables (adapter argument, DRED arming, feature level)
>    separates the successful rows from the failing ones, using the table in section 2.
> 3. Make the corresponding change in `src/dlss5-feed.cpp` — `FeedCreatePrivateDevice` and
>    `InitSession` are the relevant functions — build with `build.bat`, and have me retest.
>    Iterate until NGX initialises in-process.
> 4. If no combination succeeds, work through section 3 instead: identify what else in the
>    process is hooking NGX, and confirm against `dlss5-feed-host64.exe --test --hide` as the
>    control.
>
> Constraints: do not change the wire protocol or the IPC version. Keep the existing fallbacks
> — the data-path sweep in `SafeNgxInit12` and the DRED-disarmed retry in
> `FeedCreatePrivateDevice` — rather than replacing them. Source files are UTF-8 with BOM and
> CRLF; do not rewrite line endings. When it works, tell me exactly which combination fixed it
> and what the log line looks like, so I can post that on the issue.
