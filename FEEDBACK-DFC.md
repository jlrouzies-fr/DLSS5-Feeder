# Feature request: a compatibility mode for DLSS-contract feeders

*To: Alexander, author of Deep Fried Chicken (1.3.0-alpha)*
*From: Jean-Laurent, maintainer of `dlss5-feed`*

First: Deep Fried Chicken is impressive work, and this is not a complaint —
1.3.0-alpha does exactly what its README says. This is a request to widen the
set of games it can do it in, plus a small mutual-exclusion courtesy for the
cases where that isn't wanted.

## Who is asking

`dlss5-feed` is a ReShade add-on for games that have **no DLSS of their own**.
It manufactures the native-looking NGX contract those games are missing: it
issues `NVSDK_NGX_D3D12_CreateFeature` / `EvaluateFeature` for feature 1
(DLSS Super Resolution) every frame, with the full vocabulary a real game
supplies — Color, Depth, Motion Vectors, jitter, exposure, subrects, create
flags, quality enum, output dimensions. All four graphics backends
(D3D11/D3D12/Vulkan/OpenGL) funnel into those D3D12 NGX calls; 32-bit games
are served by a companion 64-bit host process with its own D3D12 device.

Today the downstream consumer of that synthetic contract is a neural
renderer that detours the NGX entry points. Which is the point:

## The opportunity

Deep Fried Chicken's owned path attaches to a game's **native** feature-1
evaluation — so today it lights up only in games that already ship DLSS SR
on D3D12. A feeder produces *exactly the thing you already intercept*. With
a modest compatibility mode, Deep Fried Chicken would run its passes in any
game ReShade runs in, not just the ones with native DLSS: the feeder
supplies the contract, DFC owns the neural side. We would happily be that
supplier and would rather feed you than fight you.

## Where the two currently fight

Everything below is from your shipped docs and the add-on's own log strings,
observed while assessing coexistence — no source was available to us.

1. **Detour collision.** Your entry detours are installed process-wide and
   persistently (`"standalone provider armed: persistent native-NGX entry
   detours with resolver fallback"`). Our synthetic feature-1 create is
   indistinguishable from a game's, so you would adopt it — and if the user
   also has another neural provider installed (your QUICK-START's removal
   list), two detour chains land on the same exports in load order.
2. **Loader-import patching.** `"standalone provider patched %u named
   loader/resolver import(s) in %ls"` — our own `GetProcAddress` for NGX
   symbols can silently return your wrappers, so we can't even reliably tell
   whether we're talking to the driver or to you.
3. **Lifetime budgets.** `"feature-pair lifetime budget (%d) is exhausted"`
   (4) and `"standalone FP16 codec contract budget (%d) exhausted"` (16).
   A real game creates its DLSS feature approximately once; a feeder
   recreates it on every resolution change, on periodic history resets, and
   on warm-up rebuilds. Against a feeder your budgets exhaust silently in
   minutes and the passes stop.
4. **Host pinning.** `"standalone NGX detour rejected a second native
   host"` and the DriverStore-only `_nvngx.dll` allowlist — reasonable
   against injection, but they assume the game loaded NGX, not a feeder.
5. **No negotiation channel.** The README is explicit that there is no
   provider contract, and we found no mutex, export, or documented key to
   ask you to stand down or to announce ourselves. So the only safe
   behavior available to either side is "detect the other and stop."

For what it's worth, that is what we ship meanwhile: we detect
`deep-fried-chicken.addon64`, stop feeding, and tell the user
"one or the other, never both" — mirroring your QUICK-START guidance.

## The asks, smallest first

**Tier 0 — keep the detection surface stable (zero code).**
Your `NAME` data export ("Deep Fried Chicken <version>") is what we key on
to stand down cleanly. Please treat it as a stable interface. A line in the
README blessing it for that purpose would let every feeder and provider do
polite mutual exclusion today.

**Tier 1 — a disarm switch and a minimal provider contract (small).**
`enabled=0` in `deep-fried-chicken.cfg` gates the passes but, as far as we
can observe, the Detours still install at load. A documented `arm=0` key
that prevents detour installation entirely would give users a real
per-game off switch. Beyond that: a documented "who owns feature-1
interception in this process" marker — a named mutex, a data export, or an
ini key (your bridge already reads/writes `[RenoDX.DLSS5]`, so there is
precedent) — first-comer wins, the other logs and stands down. We will
implement our side of whatever shape you pick.

**Tier 2 — feeder mode (the actual feature).**
Let a feeder mark its synthetic contract as such — e.g. a documented NGX
parameter set at CreateFeature time (`"Feeder.Provider" = "dlss5-feed"`, or
any key you choose), or recognition of the calling module. When the marker
is present:

- **Scale or lift the budgets.** Feature recreation is a feeder's normal
  breathing, not a leak. Configurable budgets would be enough.
- **Exempt the feeder from loader-import patching**, or expose a
  real-entry passthrough (your resolver already knows `real=%p` vs
  `wrapper=%p`), so the feeder can address the driver deliberately.
- **Document cadence expectations.** In-game we can guarantee one evaluate
  per Present; our 32-bit host already runs exactly one Present per
  evaluate because another downstream consumer required it. If DFC has
  per-frame assumptions, we will honor them — they just need stating.
- Optionally, **accept a vouched game-local `_nvngx.dll`** when a marked
  feeder is the one who loaded it.

## What you get out of it

- Deep Fried Chicken in every game a feeder supports — DLSS-less games are
  precisely where a 10-pass neural pipeline has the most room to shine.
- A test partner: we already do detect-and-report interop with Alex's
  Toolkit (we surface its cascade state in our overlay), we ship a
  reproducible test matrix, and we will run your alphas against our four
  backends and the 32-bit host path.

Happy to discuss any of this, adjust our side first, or test a build.
Thanks for reading — and for the add-on.
