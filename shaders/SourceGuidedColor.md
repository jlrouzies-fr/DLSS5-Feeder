# Optional source-guided colour refinement (SDR D3D11)

`SourceGuidedColor.fx` is an opt-in companion effect, not a change to the Feeder
transport or to the closed neural-rendering model. It refines the colour and
lightness base of the **whole frame** using colour affinities from the image
before Feeder, while retaining a controllable neural residual. It does not mask
out the HUD, paste original text, or recognise a fixed blue/white palette.

## Scope and setup

Requires a working Feeder/consumer installation. This effect does not install,
enable, repair or distribute any consumer, driver or NGX runtime.

Supported opt-in profile: ReShade 6.8, **D3D11, SDR sRGB, 8-bit backbuffer**.
The compile guard emits no techniques, textures or samplers by default, or for
other colour spaces/bit depths/renderers. An absent technique on HDR or another
renderer is intentional: do not force the defines to bypass the guard.

1. Back up the current preset, then add `SourceGuidedColor.fx` to the existing
   shader search path. Leave the working Feeder setup unchanged.
2. Set the effect preprocessor definition `SOURCE_GUIDED_COLOR=1`.
3. Enable both new techniques and put them immediately around Feeder:

   `motion provider(s) -> SourceGuided_Save -> DLSS5_Feed -> SourceGuided_Apply -> later effects`

4. Enable **Enable source-guided colours** in this effect's settings.
5. Start with colour separation **0.90**, tone **1.00**, detail **1.00**, and
   comparison view **Processed**. These ordinary values are saved in the preset.

Keep other colour-changing effects outside the Save/Feed/Apply interval: otherwise
their changes become part of what the refinement compares. Do not replace the
entire preset or remove an existing motion-vector provider.

After running the Feeder installer again or updating the installation, re-check
this order explicitly. The installer can move the provider and `DLSS5_Feed`
after existing effects; restore `SourceGuided_Save -> DLSS5_Feed -> SourceGuided_Apply`
before using this companion again. The installer is not changed by this effect.

| Control | Meaning |
|---|---|
| Enable source-guided colours | Refinement only; does not toggle NR |
| Colour separation, 0–1 | Master blend of the source-guided correction; 0 bypasses it |
| Tone and contrast correction, 0–1 | Scales the lightness-base correction within that blend |
| Neural residual detail, 0–2 | 1 retains the neural residual; larger values can amplify noise/halos |
| Processed | Refined output |
| Source before Feeder | Reconstructed source RGB, with post-Feeder alpha |
| Neural / post Feeder | Post-Feeder input without the new refinement |

The shader declares an F7 temporary-bypass binding. Check for a game-key conflict;
its synthetic-key activation was not confirmed during the local game test. Use
the enable checkbox/comparison selector for an unambiguous manual comparison.
The existing consumer's NR toggle is separate. Runtime bypass retains the shader's
buffers/passes; return the macro to **0** to compile them out. Disabling both
techniques stops their execution but is not a promise of immediate memory release.

## Algorithm and boundary

C0 is the frame before the **whole Feeder chain**, and N is the frame after it.
C0 is not captured inside the consumer immediately before its NR evaluation.
Scene and already-composited UI are both processed; true semantic layers cannot
be recovered from this flat pair by this filter.

Both images are converted from encoded sRGB to Oklab. A radius-4 (9×9) joint
bilateral filter computes their bases using the **same weights derived from C0**:
spatial sigma 2.6, lightness sigma 0.10, chroma sigma 0.055, clamp-to-edge samples.
Nearby areas therefore need not share a base merely because N has made their
colours similar. The weights are continuous, not hard palette classes.

Let Bs and Bn be these bases, s the master strength, t tone strength and d detail:

`F_lab = N_lab + s * (t * (Bs.L - Bn.L), Bs.ab - Bn.ab) + s * (d - 1) * (N_lab - Bn)`

The selected B++ values are s=0.9, t=1, d=1. A 16-step radial chroma compression
keeps the result inside the SDR gamut after a lightness clamp. It is not CSS
perceptual gamut mapping. Post-Feeder alpha is preserved.

A float-exact frame stamp in the source buffer rejects a missing or stale Save.
It fails closed above frame 16777214; restart the effect runtime then. This checks
only the companion source buffer, **not** whether the closed consumer produced
a fresh, successful NR frame. Confirm NR separately with its own overlay/counters.

No extra NGX evaluate, temporal history, motion/depth rewriting, installer change,
consumer patch or C++ hook is introduced.

## Contributor-local CPU checks

The Python/NumPy reference and synthetic suite are retained by the contributor
but are **not included in this contribution**. This keeps the upstream change
limited to the shader and this document, without adding a Python maintenance
requirement or a build/runtime dependency.

The 25 local tests check colour-conversion values, finite/bounded output, exact
zero-strength bypass, multiple colours/contaminants, edge gradients and retention
of synthetic luminance/chromatic detail. They do **not** execute the FX or prove
game quality. Their reported results are not reproducible from this repository
alone and are not project CI results. The shader uses the fixed radius-4 profile
and the selected B++ settings documented above.

## Contributor observations — not CI guarantees

The exact FX below was tested with the ReShade 6.8 compiler and a local isolated
D3D11 harness: 34/34 WARP and 34/34 RTX cases passed, including alpha, bypass,
source freshness/range and comparison views. CPU parity tolerance was fixed at
1/255 beforehand; maximum errors were approximately 0.00003649 / 0.00003379.
Seven default/off/unsupported-profile compile guards emitted zero resources.
The local GPU harness and private game-derived fixtures are **not bundled** in
this small patch; these are reported contributor checks, not results reproducible
from this repository alone. To validate the actual FX, repeat an in-game check.

Bounded offline observation: Heroes of Might and Magic: Olden Era 0.80.49,
ReShade 6.8, RTX 4080 SUPER, 3440×1440 SDR D3D11, an existing Feeder/NR installation.
Three launches reached the saved map. Comparison views, preset persistence,
camera/hero selection and zoom were checked. The second launch showed ACTIVE NR
with an increasing success count; white resource text was brighter/less blue,
and gold/red colourfulness improved. No claim is made that every colour error
is removed or that the result has final subjective user acceptance.

Separate final-output screenshots were captured at different times. Only static
UI regions were compared numerically, not the animated world as a synchronous
pair. Internal consumer NR_ON/OFF captures do not include this downstream effect.
No game screenshots, raw logs or proprietary binaries are included here.

| Measurement | Observed result | Meaning |
|---|---|---|
| Full-frame isolated RTX chain | p50 1.903456 ms, p95 2.463578 ms | 5 warm-up + 30 timestamp samples, not game FPS |
| Actual ReShade Save+Apply timings | 1.082 and 1.719 ms | Two snapshots, not a percentile distribution |
| Static in-game frame windows | About 59 FPS | Not an on/off speedup or whole-session smoothness claim |
| Private textures | 151.17 MiB at 3440×1440 | Two RGBA32F full-frame images; host copies are additional |

The earlier 0.5 ms aspiration was not met. Runtime bypass is not zero-cost.
The shader digest is
`DBB37E3482E4D74285CFCB13F87554B848A9EE011C97F0A9D9ED8A68CDED5FC3`.
The PR base is newer than the locally tested Feeder installation: the existing
after-technique boundary was inspected, but the new upstream binary was not
built/deployed in that game. These observations do not certify updated-base NR
compatibility on another installation.

Update check on 2026-09-06: rebased onto development branch `v0.14.0` at
`26c002d5156d178c2db438327194077c9ad94418` (includes beta.4). All 25 retained local
CPU tests passed again with Python 3.12.14 / NumPy 1.26.4; the unchanged FX checked out
in that tree passed fresh ReShade 6.8 HLSL SM5 compilation and all seven compile
guards. The D3D11 after-technique boundary and installer/release exclusion were
inspected at that revision. This was not a new GPU execution or game test of the
updated Feeder binaries; the runtime observations above remain historical.

## Limitations and reviewer checklist

- Broad, desirable NR colour/lighting changes can also be attenuated. Neural
  high-frequency colour errors can survive as residual detail.
- Missing/incorrect upstream depth or motion guides are not fixed by this effect.
  Local sampled guide warnings remained; increasing NR counters alone do not
  demonstrate correct temporal reconstruction.
- Long active play, combat, all UI transitions, reload/resize, other resolutions,
  HDR, D3D12/Vulkan/other APIs and other games remain unverified.
- No universal semantic separation, ground-truth colour recovery, model upgrade
  or guaranteed eye-comfort claim. The effect is intentionally off by default.
- Reviewer: back up a working offline preset; confirm actual NR activity, both
  ordered techniques, all three views, coloured/neutral UI, animated edges and
  camera motion. Compare live GPU timings without screenshot saving. Restart to
  check persistence. Stop on corruption/crash/flicker; disable only this effect
  and restore the saved preset. Do not overwrite a working install for this test.

## Attribution and contribution boundary

Original AI-assisted shader implementation and documentation, prepared for contribution
under this repository's MIT licence. No author signature or external endorsement
is implied. The maintainer's AI declaration is not rewritten by this contribution.

- Oklab coefficients: [Björn Ottosson](https://bottosson.github.io/posts/oklab/),
  2021 coefficient update; public-domain option, attribution retained in code.
- sRGB transfer function: [W3C CSS Color 4](https://www.w3.org/TR/css-color-4/#color-conversion-code).
- Host/FX semantics: [ReShade FX reference](https://github.com/crosire/reshade-shaders/blob/slim/REFERENCE.md)
  and ReShade 6.8 runtime; no ReShade compiler code is included in this patch.
- NumPy was used only for contributor-local checks; neither it nor Python tests
  are included. There are no media assets, models or game-specific fixtures here.
