# Detroit: Become Human (Demo) — 64-bit Vulkan copy-home stays stale (unresolved)

> ## Status — open, but there is now a lead: this looks like issue #10, not a Detroit-specific bug
>
> **New data point (2026-09-01, from the user): enabling NVIDIA Smooth Motion in DOOM 2016
> (64-bit Vulkan) reproduces the Detroit symptom exactly** — in a game that is otherwise a proven
> working row. Detroit shows it without Smooth Motion having been deliberately enabled. See
> "The Smooth Motion clue" below: it reframes everything under it, supplies the Vulkan data point
> [`PLAN-SMOOTHMOTION.md`](PLAN-SMOOTHMOTION.md) was explicitly waiting on for
> [#10](https://github.com/jlrouzies-fr/DLSS5-Feeder/issues/10), and invalidates the reasoning
> that ruled Smooth Motion out for Detroit in the first place.
>
> `enabled=0` is set in the deployed `dlss5-feed.cfg` next to the demo's exe so the game plays
> cleanly. Eight independent tests (below) all reproduce the same symptom and rule out —
> individually — transport, format, HDR, NR, DLSS itself, image import, and queue-family
> ownership. What none of them touched is **what happens to the frame after we write it**, which
> is exactly where Smooth Motion operates. The diagnostic scaffolding (four new `dlss5-feed.cfg`
> knobs, two probes) stays in the code for whoever picks this up next; see "Diagnostic tools now
> in the codebase" below.

## The report

User-confirmed, playing the game with DLSS 5 neural rendering enabled: the displayed image
freezes on an old frame and stays there far longer than any single stale frame would explain —
not a single stuck frame, but what reads like the display alternating between (or holding) very
old content while, underneath, DLSS is "working non-stop" (the DLSS 5 add-on and this feeder
both report frames evaluated and delivered at a normal rate the whole time).

**Target**: Detroit: Become Human (Demo), `E:\SteamLibrary\steamapps\common\Detroit Become Human
Demo\DetroitBecomeHuman.exe`. PE32+ x86-64, **Vulkan** (`vkCreateInstance`/`vkCreateSwapchainKHR`
in `ReShade.log`, not DXGI), 3840x2160, swapchain `minImageCount=3`, `presentMode=2` (FIFO),
`VK_FORMAT_A2B10G10R10_UNORM_PACK32` / `VK_COLOR_SPACE_HDR10_ST2084_EXT` once HDR is on (plain
`VK_FORMAT_B8G8R8A8_UNORM` SDR otherwise — both formats reproduce it). GPU: RTX 5090, driver
616.56. So this exercises the **D3D12-session-over-Vulkan-transport** path
(`src/feed_vk.h` + `src/dlss5-feed.cpp`'s Vulkan-transport block), the same code every other
Vulkan title (DOOM 2016) has run clean on.

## Hypotheses ruled out, in the order tested

Each row is one deploy-and-play cycle; `dlss5-feed.log` / `ReShade.log` evidence is the primary
source, cross-checked against what the user saw on screen.

1. **Two-frame flicker on the very first run (SDR, before any of the below).** Consistent with
   converged/stuck DLSS history at first glance. Superseded by the frozen-image report below,
   which reproduces with `NeuralUplift=0` (no DLSS history at all) — so this was probably the
   same underlying bug, not a separate one. Never independently re-isolated; worth revisiting if
   the root cause is ever found and this data point still doesn't fit.
2. **`IsHdrFormat()` missing `R10G10B10A2_UNORM`/HDR10** (`src/dlss5-feed.cpp:973` at the time).
   Real bug — the format-only heuristic can't distinguish 10-bit SDR from HDR10, and the feature
   was genuinely being built `flags=74 (SDR ...)` while the swapchain was PQ/HDR10. Fixed by
   setting `hdr=1` in `dlss5-feed.cfg`. **No change to the freeze.** Ruled out as *the* cause,
   though the underlying format gap is real and still worth a proper colour-space-keyed fix
   later (see "Still worth doing" below).
3. **RenoDX NR interposition** (`renodx-dlss5.addon64`'s own neural-rendering pass, layered on
   top of plain DLAA). Set `NeuralUplift=0` in `ReShade.ini`'s `[RenoDX.DLSS5]` section —
   confirmed in the log (`NeuralUplift=0 (user-set; leaving it alone)`), 1800 frames delivered,
   zero evaluate failures. **Still frozen.** NR ruled out; this is plain DLAA through the feeder.
4. **DLSS temporal history stuck/converged.** Set `reset_every=1` (forces `InReset=1` on every
   evaluate, discards history every frame). Result: image "resets here and there" but is not
   live — **partial** unfreezing, not full. Tells us DLSS's *output* does track a changing input
   some of the time, which reframed the hunt onto "why does the input/output only sometimes
   reach the screen" rather than "DLSS is stuck".
5. **Vulkan↔D3D12 queue-family ownership never transferred.** The imported images are
   `VK_SHARING_MODE_EXCLUSIVE`; per the Vulkan external-memory spec, ownership must be released
   to `VK_QUEUE_FAMILY_EXTERNAL` before the other API touches an exclusive-mode image and
   re-acquired before Vulkan touches it again, and the codebase had no such transfer anywhere.
   Implemented `FeedVkExternalTransfer` (`src/feed_vk.h`) plus a `vkCreateDevice`-hook capture of
   the graphics queue family (`src/feed_vk_hook.h`), release before the D3D12 hand-off and
   acquire after the out-fence wait, in both the 64-bit and 32-bit-DXVK per-frame paths. This is
   spec-correct and was a real gap — kept regardless (committed `ba93726`). **No change to the
   freeze.**
6. **The imported OUTPUT *image*'s memory doesn't stay coherent across the API boundary on this
   driver/format.** Added a `buffer_home` path: route the output hop through a shared **linear
   D3D12 buffer** (`VK_BUFFER_USAGE_TRANSFER_*`, imported as a `VkBuffer`) instead of the
   imported `VkImage` — no opaque tiling/compression metadata to fall out of sync. **No change.**
7. **Same theory, input direction too.** Extended `buffer_home` to route all four inputs (colour,
   depth, MV, mask) through shared linear buffers as well, D3D12 re-filling the evaluate's actual
   input *textures* from those buffers each frame. **No change.**
8. **DLSS/NGX itself is where the staleness originates.** Added `passthrough=1`: keeps every
   byte of the transport (copy-in, fence hand-off, D3D12 round trip, buffer or image copy-home)
   identical, but swaps the NGX evaluate for a plain `CopyResource(OUTPUT <- COLOR)`. **Still
   froze, same as with real DLSS.** This is the most conclusive test run: it isolates the fault
   to somewhere in copy-in → D3D12 round trip → copy-home, with DLSS entirely out of the loop.

## The two probes, and what they say (and don't)

Both are live in `src/dlss5-feed.cpp`, gated to run every 60 frames, log-only:

- **Stale probe** (`StaleProbeRecord`/`StaleProbeAnalyse`): hashes a 64×64 centre block of the
  D3D12 *colour input* and *output* textures right after each evaluate, and reports whether each
  changed since the previous probe. **Every single probe across every run reported both
  `changed`.** So on the D3D12 side, by the time the evaluate runs, the input texture is
  receiving fresh bytes every 60-frame sample, and the evaluate (or, in `passthrough=1`, the
  plain copy) is producing a fresh output every time.
- **Sync probe**: every 60 frames, logs `sig`/`wait` (ReShade's own `queue->signal()` /
  `queue->wait()` booleans on the imported-fence-backed `fence`), plus the raw D3D12
  `GetCompletedValue()` and the Vulkan timeline-semaphore value read back through the import, for
  both the in-fence and out-fence. **Every sample across every run showed `sig=1 wait=1`, and all
  four counters tracking the frame number in lockstep** (e.g. `d12 in=180 out=180 | vk in=180
  out=180` at `n=181`). By these numbers the cross-API fence import and ordering are healthy.

**That combination is the open puzzle.** Both probes say the D3D12 side is fresh every frame and
the fences agree on completion every frame — yet `passthrough=1` (test 8) proves the visible
result is stale regardless of what runs on the D3D12 side. Two readings that don't yet
reconcile:

- The probes only prove the *D3D12-side resource* is fresh at the moment they sample it (every
  60th frame, read from the D3D12 device's own view). They say nothing about whether the
  **Vulkan-side backbuffer image the copy-home writes into is the one actually being presented**
  this frame. A `minImageCount=3` FIFO swapchain cycles through three images; if
  `dev_api->get_resource_from_view(rtv)` (`src/dlss5-feed.cpp`, Vulkan-transport block) is
  handing back a resource/index that has drifted out of sync with what the compositor is really
  scanning out — most plausible right after one of the swapchain-recreate events already visible
  in the log (loading-screen 1024×576 runtime, then the real 3840×2160 one; a warm-up rebuild at
  frame 180) — every write could be genuinely fresh and every fence genuinely honoured, while the
  *displayed* image is an old cycle of the same three-image ring. This would also explain "very
  old frames staying on display for way too long" better than a simple pacing stall: it reads
  like a frozen frame precisely because the same stale image index keeps getting re-presented.
- Alternatively, ReShade's own effect-chain scheduling around a Vulkan swapchain with `flags=0x4`
  (mutable format) could be doing something the D3D11/D3D12 paths never exercise. Every proven
  Vulkan game so far (DOOM 2016) is `B8G8R8A8_UNORM`/`FIFO`, not this swapchain's mix of
  10-bit/HDR10 formats and mutable-format flag.

## The Smooth Motion clue (2026-09-01) — the current leading theory

**The observation.** Turning NVIDIA Smooth Motion on in **DOOM 2016 (64-bit Vulkan)** — the
proven-working Vulkan row in `README.md` — reproduces the Detroit symptom exactly. Detroit shows
it with Smooth Motion never deliberately enabled.

That is the [#10](https://github.com/jlrouzies-fr/DLSS5-Feeder/issues/10) reporter's case
(RDR2, 64-bit Vulkan, "everything just flickers" once Smooth Motion is on), reproduced locally,
and it is the Vulkan data point `PLAN-SMOOTHMOTION.md` says is "still needed".

**A correction, because it changes what to do next.** Earlier in this investigation, Smooth
Motion was ruled out for Detroit on the grounds that `dlss5-feed.log` carried no Smooth Motion
warning. **That inference is invalid for a Vulkan game.** `DetectSmoothMotion()`
(`src/dlss5-feed.cpp`) tests exactly one thing:

```c
if (GetModuleHandleW(L"NvPresent64.dll") == nullptr) return false;
```

`NvPresent64.dll` is the **DXGI/D3D** implementation — it hooks `CreateDXGIFactory*` and wraps
`IDXGISwapChain`. On Vulkan there is no such module: NVIDIA implements Smooth Motion **inside the
ICD's own swapchain**, and it is not a separate implicit layer either (verified on this machine —
`HKLM\SOFTWARE\Khronos\Vulkan\ImplicitLayers` lists GOG, ReShade, OBS, EOS, RTSS and Steam, and
nothing from NVIDIA). So the detector is **structurally blind on Vulkan**: it would have stayed
silent in the DOOM run *with Smooth Motion demonstrably on*, and it proves nothing about Detroit.
The absence of that warning in the Detroit logs is not evidence of absence.

**Why this fits the evidence better than anything above.** Smooth Motion presents *more often
than the game renders*, from its own pacer, holding real frames back to interpolate between them.
Every test 1–8 above instrumented **our side of the frame** — inputs, evaluate, output, fences,
memory routes — and every probe agreed our side is fresh and correctly ordered every frame.
Nothing in this investigation ever measured **what actually reaches the screen**. A pacer that
holds and re-presents frames on its own schedule sits precisely in that unmeasured gap, and it
explains the two findings that otherwise refuse to reconcile:

- Both probes clean, every frame (our writes land, fences honoured), *and*
- `passthrough=1` still stale (a plain byte copy through the same pipe still displays old
  content) — because the staleness is not produced by anything we do to the frame. It is
  produced by whoever presents it afterwards.

It also explains the "way too long" duration far better than a fixed one-or-two-frame ordering
slip, and it retro-fits data point 4 (`reset_every=1` giving *partial* unfreezing — real frames
getting through intermittently between held/generated ones).

**The open question is what plays that role in Detroit**, which the user did not knowingly
enable. Three candidates, cheapest test first:

1. **Smooth Motion is in fact active in Detroit** without having been turned on per-game — the
   "Smooth Motion - Enabled APIs" DRS bitfield (`0xB0CC0875`) **defaults to `7`, which includes
   bit `4` = Vulkan**, so a global/NVIDIA App enable, or a shipped per-app profile, would apply
   silently. Nothing in our logs could tell us, per the correction above.
2. **Another present-path interposer.** This machine has five implicit Vulkan layers loaded into
   every Vulkan app (GOG Galaxy overlay, OBS, EOS overlay, **RTSS**, Steam overlay). RTSS in
   particular is documented in `PLAN-SMOOTHMOTION.md` as switching its presentation handling when
   it detects a pacer. DOOM works with all of these present and Smooth Motion off, so none is
   sufficient alone — but one of them may behave differently against Detroit's HDR10 /
   mutable-format / 3-image swapchain.
3. **The game's own presentation** doing something pacer-shaped natively.

Note also a **stale implicit-layer registration** found while checking: `HKCU\...\ImplicitLayers`
points at `G:\Games\Ryujing-DLSS\dlss5-vk-bridge.json`, which **does not exist on disk**. The
loader skips a dangling manifest, so it is not the cause, but it should be cleaned up so it
cannot confuse a future Vulkan investigation.

## What to check next

Reordered around the Smooth Motion clue. The first two are minutes of work and could close this
out; everything after assumes they came back negative.

1. **Is Smooth Motion actually running in Detroit? Turn on "Smooth Motion - Debug Bars"**
   (DRS `0xB01B8B02`, NVIDIA Profile Inspector) and launch. It draws coloured bars on generated
   frames. **Bars present = answered**: Detroit has Smooth Motion on without you enabling it,
   this is [#10](https://github.com/jlrouzies-fr/DLSS5-Feeder/issues/10), and Detroit stops being
   a separate investigation. No logs, no build, purely visual.
2. **Then turn Smooth Motion off for Vulkan only and retest both games** — "Smooth Motion -
   Enabled APIs" (`0xB0CC0875`), clear bit `4` (leaving DX11/DX12 alone), or "Smooth Motion -
   Enable" (`0xB0D384C0`) `= 0` on the Detroit profile specifically. If Detroit comes good, the
   root cause is confirmed and the remaining work is all in `PLAN-SMOOTHMOTION.md`'s scope, not
   here. If DOOM comes good but Detroit does not, Detroit has a *second* interposer of the same
   shape and candidates 2–3 in the section above are next.
3. **Count presents against feed frames.** Hook `vkQueuePresentKHR` the way `feed_vk_hook.h`
   already hooks `vkCreateDevice`, and log the present count against `g.frames_done` every 60
   frames. **`presents > feeds` is the pacer signature** — it says frames are reaching the
   display that we never touched, which is both the mechanism and, generalised, a *better
   Smooth Motion detector than the module-name check* (see "Still worth doing"). The same hook
   gives `pImageIndices` for free, which answers the swapchain-ring question below in the same
   run — one hook, both theories.
4. **A visible per-frame marker**: have the copy-home write a solid colour block in a corner that
   alternates by `frame_n & 1` (or a small binary frame counter), instead of trusting probes. If
   the marker on screen holds or skips while the log says frames are delivered, the fault is
   downstream of us with zero ambiguity — and filming/step-framing it shows exactly *how* the
   sequence is being reordered or held, which distinguishes a pacer (regular held/generated
   pattern) from a ring desync (index cycling wrong).
5. **Rule out the other interposers**: close RTSS, OBS, GOG Galaxy and the Steam overlay (or
   temporarily unregister their implicit layers) and retest Detroit. Cheap, and candidate 2 in
   the section above names RTSS specifically.
6. **Confirm Present is being called normally at all.** Does the ReShade overlay (Home key) —
   FPS counter, cursor, animated elements — update live while the game view is frozen? If yes,
   Present fires every frame and only the *content* is stale. If the overlay is frozen too, the
   whole presentation pipeline is stalled and every finding above needs re-reading. (This was
   never actually confirmed and is a one-second check.)
7. **Try forcing a smaller `minImageCount`** (2, intercepting the swapchain create info the way
   `vkCreateDevice` is already intercepted) or a different present mode. A ring-desync theory
   predicts the lag changes with ring size; a pacer or a pure ordering bug does not.
8. **Try another Vulkan game with a similar swapchain** (10-bit/HDR10, mutable format,
   `minImageCount=3`), Smooth Motion confirmed off, to separate "Detroit's swapchain shape" from
   "Detroit specifically". No working Vulkan row in `README.md`'s Status table matches this
   swapchain shape.
9. **Watch whether the staleness duration is constant or grows** across a long session
   (`passthrough=1` + `half_home=1`). Unbounded growth points at an accumulating queue (pacer or
   ring desync); a fixed lag points back at ordering.

## Diagnostic tools now in the codebase (kept for the next session)

All in `src/dlss5-feed.cpp` / `src/feed_vk.h`, all off by default except `buffer_home` (harmless
elsewhere, and a real spec-correctness fix regardless of whether it turns out sufficient here):

| `dlss5-feed.cfg` key | Default | What it does |
| --- | --- | --- |
| `buffer_home` | `1` | Routes the Vulkan-transport copy-home (and, if it built, the inputs too) through shared linear D3D12 buffers instead of the imported images. Live-reloads (rebuild trigger). |
| `half_home` | `0` | Diagnostic: the mode-2 copy-home paints only the **left half** of the frame, leaving the right half as whatever ReShade/the game already put there — an unambiguous split-screen "feeder output" vs. "live game" comparison, the mode-1 trick extended to the full DLSS path. |
| `passthrough` | `0` | Diagnostic: swaps the NGX evaluate for a plain `CopyResource(OUTPUT <- COLOR)` on the same D3D12 list — identical transport, no DLSS. Requires `color_fmt == output_fmt` (an `sprintf`-format check already covers the mismatch case). |
| `reset_every` | `0` | (Pre-existing.) Forces `InReset=1` on every evaluate — throws away DLSS temporal history every frame. |

Plus two always-on, log-only probes (no config needed, both sample every 60 frames):

- **Stale probe** — `StaleProbeRecord`/`StaleProbeAnalyse` in `src/dlss5-feed.cpp`. Logs
  `stale probe (frame N): colour-in {SAME|changed} (hash), output {SAME|changed} (hash)`.
- **Sync probe** — inline in the Vulkan-transport per-frame function. Logs
  `sync probe: n=N sig=B wait=B | d12 in=X out=Y | vk in=X out=Y`.

Both are cheap (a 64×64 readback / a semaphore query) and safe to leave on in any deploy; they
only add log lines.

## Still worth doing regardless of this bug

- **`DetectSmoothMotion()` is blind on Vulkan and OpenGL** — it checks for `NvPresent64.dll`,
  which only exists on the DXGI/D3D path (see "The Smooth Motion clue"). Every Vulkan user hits
  #10 with no warning at all, and this investigation lost hours to reading that silence as
  "Smooth Motion is off". The module check should stay as a fast positive, but the *real* fix is
  behavioural and vendor-agnostic: **compare `vkQueuePresentKHR` count against feed frames** (or
  the equivalent per API) and warn when presents meaningfully outnumber fed frames, which
  catches any external pacer regardless of vendor, API or module name. Item 3 in "What to check
  next" builds exactly this instrumentation; promoting it from probe to shipped detector is a
  small step afterwards. Until then, `README.md`'s Smooth Motion warning should say explicitly
  that the runtime detection does not work on Vulkan.
- **Clean up the dangling implicit Vulkan layer** on this machine:
  `HKCU\SOFTWARE\Khronos\Vulkan\ImplicitLayers` registers
  `G:\Games\Ryujing-DLSS\dlss5-vk-bridge.json`, which no longer exists on disk.
- **`IsHdrFormat()` (`src/dlss5-feed.cpp`) should key off the Vulkan swapchain's actual
  `imageColorSpace`**, not guess from the DXGI format alone — `R10G10B10A2_UNORM` is legitimately
  either 10-bit SDR or HDR10 depending on colour space, and the format-only heuristic will
  mis-classify one of them on some other game even after this bug is understood. Not urgent on
  its own (item 2 above showed it isn't *this* bug), but a correctness gap worth closing.
- The `git log` for this investigation is messier than it should be: the working-tree state that
  became commits `ba93726` and `14dacbf` was picked up by a different, concurrently running
  session on this same checkout (branch is now `mergeAndMore`, not the `beta/v0.8.0-beta.4` this
  investigation started on) and swept in alongside unrelated v4.7-support work rather than being
  committed as its own topic. The diagnostic scaffolding above is real and correct, but its
  commit message (`14dacbf`) doesn't mention any of it. Worth a follow-up commit that documents
  what's actually in the tree, or a `git log -p` read-through before trusting either commit
  message at face value.
