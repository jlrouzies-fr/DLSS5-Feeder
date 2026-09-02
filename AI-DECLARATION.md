---
version: "0.1.2"
level: copilot
processes:
  design: pair
  implementation: copilot
  testing: pair
  documentation: copilot
  review: pair
  deployment: pair
components:
  src: copilot
  host: copilot
  layer: copilot
  shaders: copilot
  spike: copilot
  .github/workflows: copilot
  README.md: copilot
---

This format is based on [AI-DECLARATION.md](https://ai-declaration.md/en/0.1.2).

## Notes from myself

DLSS5-Feeder is AI-written under human direction, and it would be misleading to present it any
other way.

I am a software engineer and IT administrator. I am not a graphics programmer — no D3D12, no
Vulkan, no NGX, no ReShade add-on work before this repository's first commit. What I claim is the
direction and the proving: deciding that a game which ships no DLSS should be handed a DLSS
contract anyway, choosing which API or bitness to chase next, calling when a path is dead — and
then every hour spent installing each build into a real game on real hardware and looking at what
came back. That last part has no shortcut, and it is the only place any of this is proven.

I take no credit for what this stands on. DLSS and NGX are NVIDIA's, the neural rendering is the
RenoDX community's add-on, ReShade is Patrick Mours', the D3D11↔D3D12 shared-texture and fence
transport is adapted from NIGos' `dlss5-dx11-bridge`, and the DirectX 9 path only exists because
of Dege's dgVoodoo2. See [Credits](README.md#credits).

## AI Notes

What that split looks like in practice:

- **The author owns every decision and all judgement of the result.** Which API to support next,
  which game to try, whether a symptom is worth another deploy-and-play cycle, what to ship and
  what to hold. There is no test rig for this project: DLSS needs an RTX GPU, an NGX runtime and a
  real swapchain, so a change is proven by installing it into a game and playing it. The CI
  workflow says so in its own comments — *"a green tick here means 'it compiles and links',
  nothing more; the games table in the README still has to be earned by hand."*
- **That direction is technical, not just "the picture looks wrong".** Working from the rendered
  output, the logs, and a broad knowledge of how these parts behave — depth buffers, upscalers and
  their history, HDR and colour spaces, frame pacers, what a given knob is *supposed* to do — the
  author has repeatedly redirected the AI to a better implementation than the one it proposed, and
  rejected changes that were correct in isolation and wrong in the frame.
- **Claude owns the writing.** Every line in `src/`, `host/`, `layer/`, `shaders/` and `spike/`,
  the CI workflow, and all of the documentation — the README, the `PLAN-*.md` investigations, the
  deployment runbook, and this file. It also proposes designs, investigates faults, and reports
  what it verified versus what it assumed. Nearly every commit it wrote carries a
  `Co-Authored-By: Claude` trailer, so the split is auditable per commit with `git log`; the
  exceptions on this branch are a merge, a revert, two small hand edits, and the commits from
  outside contributors.
- **Testing is genuinely shared.** The interop spikes, the probes, the overlay diagnostics and the
  CI are Claude's. Running the games, reading what actually reached the screen and catching the
  regressions is the author's, and that is the half that has reliably found the faults.
- **`deployment: pair`** — the build scripts, the CI and `DEPLOY-DEV.md` are AI-written; the
  builds, the installs into each game and the releases are run and verified by the author.

The standing rule this project works by: **a clean log from instrumentation the AI just wrote is
weaker evidence than what the person watching the screen says. When they disagree, distrust the
tool.** The cases behind it:

- For several releases the feed ran on **zero motion vectors**. The recommended provider
  (ReshadeMotionEstimation) does not compile on ReShade 6.8, but ReShade still lists the technique
  as enabled, so nothing anywhere looked wrong. The add-on now reads the compiler's own line out
  of `ReShade.log` and reports it — a check that exists only because the failure was invisible
  from the inside.
- **Every D3D11 work resolution below 100% did nothing at all**, while the log printed a
  reassuring `ColorInput found`. ReShade had resolved the semantic texture *variable* but bound no
  view behind it; the frame gate needed that view and silently bailed, so DLSS disappeared
  entirely and only 100% ever worked.
- **Detroit: Become Human** held a stale picture while this feeder and the neural add-on both
  reported frames evaluated and delivered at a normal rate. Eight deploy-and-play cycles — one per
  hypothesis, covering transport, format, HDR, NR, DLSS itself, image import and queue-family
  ownership — all came back clean, because the cause was outside anything the code can see: an
  in-driver frame pacer building its generated frames from a source captured before the effect
  chain runs. The test shader written to settle it was itself misread on its first run (a
  saturating tint is invisible on a bright centre, and a one-frame alternating marker looks steady
  at 100+ fps). The AI's own instrument lied twice before it told the truth. See
  [`PLAN-DETROIT.md`](PLAN-DETROIT.md).

This repository is code, shaders and documentation. **Nothing in it is generated media** — there
is no art, no audio, no synthesised asset of any kind. `external/` holds third-party headers and
SDKs (ReShade, NGX, Vulkan, MinHook, ImGui) that are the work of their own authors and are not
covered by this declaration; `deploy/` is an untracked local cache, documented in
[`deploy/SOURCES.md`](deploy/SOURCES.md). Where the feeder has to interoperate with a
closed-source add-on, what it knows comes from that add-on's shipped documentation, its own log
strings and its observable behaviour — not from decompiled or leaked sources.

This declaration covers the maintainer's own work. Pull requests and commits from outside
contributors are their authors' own, made under whatever arrangement they choose, and several
rows of the games table were verified by other people on their own hardware.
