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

## Resolution (2026-09-01, DOOM 2016 + Smooth Motion, branch `vulkanAndOptimization`)

DOOM 2016 with Smooth Motion on turned out to be the better test bed than Detroit — a
proven-working row that breaks only when the pacer is switched on. Three builds later the
mechanism is identified, and it is **not fixable from inside a ReShade add-on**.

**What the runs established, in order:**

1. **`async_home=1`** — copy home carries frame `n-1` and waits on fence `n-1`, so the game's
   present path holds no cross-API stall. **No change.** The stall was not the mechanism.
2. **The present hook** reported `last swapchain image index 0` on *every* present. Under the
   pacer the app is writing one image over and over, so there is no ring protecting a frame that
   is still being read. (The present *ratio* it also reports is an undercount and should not be
   trusted: the hook sits on `vulkan-1.dll`'s exported `vkQueuePresentKHR`, and an engine using
   device-level dispatch bypasses it entirely. The earlier reading of "no pacer warning fired,
   therefore presents ≈ feeds" was wrong for that reason.)
3. **`sync_home=1`** — flush and CPU-block until the copy home has actually finished on the GPU
   before the technique callback returns, i.e. before the game presents. **This changed the
   symptom**, the only thing in the whole investigation that did: from "stuck on old frames" to
   **"flicker between the correct image and the previously stuck frames"**.

4. **`SmoothMotionTest.fx`** (`shaders/`, kept) — feeder disabled, one diagnostic effect on:
   a full-frame tint, a wall-clock-driven sweeping bar, and a per-frame alternating corner
   block. Read at the time as "ReShade survives the pacer". **That reading was too confident**:
   a moving bar and a 1-frame-alternating block both look "steady" at 100+ fps even if every
   second frame drops them entirely, and the tint used a *saturating add*, which is invisible on
   an already-bright centre and reads as a vignette — exactly what was reported. The shader now
   blends instead of adding, so a future run of it means something. Treat that pass as
   inconclusive, not as evidence.
5. **`async_home=2`** — one queue submit per frame. Our two submits per frame (inputs+release,
   then a second buffer for the copy home after the cross-API fence) were the last structural
   difference between us and a normal game, and a pacer inferring frame boundaries from
   submissions would pair the wrong frames. With the copy home carrying `n-1` it no longer
   depends on this frame's evaluate, so it rides the input command buffer and the frame leaves
   as a single submission. **Symptom changed again, to the same place `sync_home` reached:**
   flicker between frames that are correctly DLSS-processed and frames that are not.

**Tests 3 and 5 converge, and that convergence is the diagnosis.** Two independent routes — force
the writes complete before present, or make the frame a single normal-shaped submission — both
end at: *real* presented frames carry our output correctly, and the frames in between do not.
Those in-between frames are the pacer's **generated** ones. The interpolator builds them from its
own source, captured at a point that does not include anything done during the effect chain. So:

- Our side can be made arbitrarily correct, arbitrarily early, and structurally
  indistinguishable from a normal game's frame — tests 3 and 5 prove all three.
- We still cannot make the driver's interpolator read what we wrote. Its source and capture point
  are internal to the ICD, below every layer, and nothing an add-on can reach.

**Net effect of the two knobs, if anyone wants the least-bad behaviour under a pacer:** the
symptom moves from *stuck on very old frames* (unusable) to *alternating processed/unprocessed
frames* (still unusable, but every real frame is correct). That is a strictly better failure
mode, not a fix, and neither knob is on by default.

**Conclusion: Vulkan + NVIDIA Smooth Motion + this feeder is not reconcilable in-process.** The
supported answer is the per-API opt-out — "Smooth Motion - Enabled APIs" (`0xB0CC0875`), clear
bit `4` (Vulkan), which leaves Smooth Motion working for D3D11/D3D12 games. Vulkan itself is
unaffected and stays a proven path: DOOM 2016 is correct with the pacer off.

**Detroit is very likely the same thing** and was never independently confirmed to have Smooth
Motion off — the Debug Bars check (item 1 below) is still the two-minute way to settle it. If the
bars appear, Detroit is not a separate bug and this document closes with it.

**The one avenue not tried**, recorded rather than pursued: doing the feed inside a Vulkan
**layer** rather than a ReShade add-on. A layer sits above the ICD, so its writes would land
before the interpolator's snapshot rather than after it. That is a second implementation of the
whole transport for an uncertain payoff, and `layer/VkLayer_feed_vk.dll` as it exists today does
**not** do this — it only hooks `vkCreateDevice` to append extensions.

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
| `async_home` | `0` | Copy home carries the previous frame's output and waits on fence `n-1`, taking the cross-API stall out of the game's present path (the home buffer is double-slotted so the two never alias). Costs one frame of latency. Did not fix the pacer case; kept as a latency/stall option worth measuring on its own. |
| `sync_home` | `0` | Diagnostic: flush and CPU-wait for the copy home to complete before the callback returns, i.e. before present. A full GPU drain per frame — never a shipping default. One of the two knobs that isolated the pacer's capture point (see Resolution). |

`async_home=2` additionally collapses the Vulkan frame to **one queue submit** (the copy home
rides the input command buffer). That is worth measuring as a plain optimization with no pacer
involved — it halves our submissions per frame — but it is unverified for latency and correctness
outside the Smooth Motion tests, which is why it is not the default.

`shaders/SmoothMotionTest.fx` is the ReShade-only diagnostic: it answers "does the effect chain's
output reach the displayed frames at all", with the feeder disabled. Its first run was misread
(see Resolution, test 4); the tint marker has since been fixed to a blend.
| `reset_every` | `0` | (Pre-existing.) Forces `InReset=1` on every evaluate — throws away DLSS temporal history every frame. |

Plus two always-on, log-only probes (no config needed, both sample every 60 frames):

- **Stale probe** — `StaleProbeRecord`/`StaleProbeAnalyse` in `src/dlss5-feed.cpp`. Logs
  `stale probe (frame N): colour-in {SAME|changed} (hash), output {SAME|changed} (hash)`.
- **Sync probe** — inline in the Vulkan-transport per-frame function. Logs
  `sync probe: n=N (waited out=M) sig=B wait=B | d12 in=X out=Y | vk in=X out=Y`.
- **Present probe / pacer detector** — `feed_vk_hook.h` hooks `vkQueuePresentKHR` and logs
  `present probe: P presents / F frames fed (R), last swapchain image index I` every 120 frames,
  warning once when presents outnumber frames fed. **Caveat: `P` is an undercount** — the hook is
  on `vulkan-1.dll`'s exported symbol, so presents dispatched through a device-level function
  pointer never reach it. Treat the *ratio* as a lower bound; the *image index* is reliable and
  was the useful half.

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
