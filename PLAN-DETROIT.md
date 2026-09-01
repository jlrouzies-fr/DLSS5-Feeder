# Detroit: Become Human (Demo) — 64-bit Vulkan copy-home stays stale (unresolved)

> ## Status — open, feed disabled for this game pending a new hypothesis
>
> `enabled=0` is set in the deployed `dlss5-feed.cfg` next to the demo's exe so the game plays
> cleanly. Eight independent tests (below) all reproduce the same symptom and have exhausted
> every hypothesis raised so far — transport, format, HDR, NR, DLSS itself, image import, and
> queue-family ownership are each individually ruled out. The diagnostic scaffolding (four new
> `dlss5-feed.cfg` knobs, two probes) stays in the code for whoever picks this up next; see
> "Diagnostic tools now in the codebase" below.

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

## What to check next

In rough order of how cheap the test is:

1. **Confirm Present is even being called normally.** Does the ReShade overlay (Home key) —
   FPS counter, cursor, any animated overlay element — update live while the game view is
   frozen? If yes, Present fires every frame and the game's own render loop is not the problem;
   the fault is specifically in which image gets shown. If the overlay is frozen too, the whole
   presentation pipeline is stalled and every finding above needs re-reading in that light.
2. **Log the actual swapchain image index / VkImage handle the copy-home writes into, every
   frame**, and separately whatever ReShade exposes for "current back buffer index" (check
   `reshade::api::effect_runtime`/`swapchain` for an index accessor, or hook
   `vkQueuePresentKHR`'s `pImageIndices` directly the way `feed_vk_hook.h` already hooks
   `vkCreateDevice`). Compare the index written against the index actually presented that frame.
   A mismatch that grows or cycles wrong would confirm the swapchain-ring-desync hypothesis in
   "what the probes don't prove" above.
3. **A visible per-frame marker**: have the copy-home XOR a 1-pixel corner block with the frame
   parity (or write a solid colour that alternates red/green by `frame_n & 1`) instead of the
   real content, with DLSS/passthrough irrelevant. If the on-screen corner marker itself lags or
   skips, that isolates the fault to presentation/image-selection with zero ambiguity — no probe
   reading required, just watching the corner.
4. **Try forcing a smaller `minImageCount`** (2, if the swapchain create info can be intercepted
   the way `vkCreateDevice` already is) or a different present mode, to see if the symptom
   changes shape — a ring-desync theory predicts the lag would change with the ring size; a pure
   fence/ordering bug would not.
5. **Try another Vulkan game with a similar swapchain** (10-bit/HDR10 colour space, mutable
   format, `minImageCount=3`) to find out whether this is Detroit-specific or a broader gap in
   the Vulkan-transport path that DOOM's simpler swapchain never exercised. None of the working
   Vulkan rows in `README.md`'s Status table match this swapchain shape.
6. **Re-run the full test 8 (`passthrough=1`) with `half_home=1` while also watching whether the
   staleness duration is constant or grows over the session** — "way too long" from the user
   reads as possibly unbounded/growing, which would point more at an accumulating desync
   (matches the ring-index theory) than at a fixed one-or-two-frame lag (which would point back
   at ordering).

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
