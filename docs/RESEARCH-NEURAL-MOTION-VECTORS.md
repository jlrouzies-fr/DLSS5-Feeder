# Research: neural models for motion vectors

Status: **shelved** (2026-11-12). Nothing from this note is in the product.

The question was whether a learned optical-flow network could beat, or improve, the built-in motion
estimator of `shaders/DLSS5_Feed.fx` (`DLSS5_MV_PROVIDER=5`). Short answer:

- No public pretrained model that could run in real time beats the built-in estimator on the rig.
- Putting a pretrained model on top of our vectors, to catch their errors, gains nothing either.
- The errors of the two ARE complementary, so a model trained for this job could help. That means
  building training data and training for weeks, which was decided against for now.

This note keeps the numbers so the question does not have to be asked again from scratch, and lists
what a training project would need if it is ever picked up.

## What was measured

- Rig: `tools/fx-testbed` (fxlab), 3D scene, 3840x2160, **no depth buffer** (the capture case), shader
  at commit `497427c`. Frame pairs, ground-truth vectors and pixel classes were dumped from a patched
  private copy of fxlab; the patch (a `--dump` option) is not in the repo.
- Five frames of the camera schedule: f100 camera still, objects move; f210 fast yaw pan (about 38 px
  a frame); f255 slow pan (under 1 px); f345 dolly; f560 strafe with yaw.
- Error = mean distance to the true vector, in 4K pixels, by the rig's own classes: textured, plain
  (no visible gradient), outline (a depth step within two pixels), HUD (static overlay; the right
  vector is zero). The offline scoring reproduced the rig's printed numbers exactly.
- Networks run on a downscaled pair (area filter) and their flow is scaled back up bilinearly.
- Hardware: RTX 5090, PyTorch 2.11, half precision where the model supports it. The GPU was shared
  with another rig session during the runs, so times are rough (minimum of several runs).

Models:

| Model | Source | Licence | Notes |
|---|---|---|---|
| DIS (Kroeger et al. 2016) | OpenCV `cv2.DISOpticalFlow` | OpenCV's | classical, CPU, at 4K |
| RAFT small / large (Teed and Deng 2020) | torchvision, default weights | code BSD-3 (from memory); weight terms not checked | large shows the ceiling, not a real-time candidate |
| NeuFlow v2 (2024, arXiv 2408.10161) | github.com/neufieldrobotics/NeuFlow_v2, `neuflow_mixed.pth` | Apache-2.0 | the only real-time candidate tested |

Not tested: SEA-RAFT (published speed about 21 fps at 1080p on an RTX 3090), any model exported to
TensorRT or ONNX Runtime, any neural consumer (DLSS-NR) downstream of the vectors.

## Result 1: the models on their own

Mean error, textured / plain, per frame. "Ours" is the raw estimator output, before the per-pixel stage.

| Method | Objects f100 | Fast pan f210 | Slow pan f255 | Dolly f345 | Strafe f560 | Time per frame |
|---|---|---|---|---|---|---|
| Ours (raw) | 0.41 / 0.41 | 0.81 / 1.40 | 0.31 / 0.30 | 1.39 / 1.49 | 1.12 / 1.03 | shader |
| DIS fast, 4K | 2.20 / 1.40 | 2.26 / 2.37 | 0.90 / 0.62 | 2.95 / 2.65 | 2.41 / 1.89 | 41 ms CPU |
| DIS medium, 4K | 6.13 / 3.00 | 2.14 / 2.39 | 0.87 / 0.57 | 3.15 / 2.53 | 2.35 / 1.75 | 167 ms CPU |
| NeuFlow v2, 960x544 | 2.16 / 3.08 | 1.26 / 1.29 | 0.30 / 0.31 | 1.89 / 4.73 | 1.27 / 1.32 | 11 ms |
| NeuFlow v2, 1280x720 | 2.88 / 4.08 | 0.87 / 0.83 | 0.41 / 0.46 | 1.32 / 7.27 | 1.05 / 0.98 | 20 ms |
| RAFT small, 1920x1080 | 1.47 / 1.42 | 0.93 / 0.90 | 0.24 / 0.33 | 1.32 / 2.48 | 0.90 / 1.21 | 79 ms |
| RAFT large, 960x544 | 1.58 / 1.82 | 0.68 / 0.67 | 0.21 / 0.27 | 0.79 / 1.62 | 0.81 / 0.81 | 48 ms |
| RAFT large, 1920x1080 | 0.79 / 0.55 | 0.45 / 0.68 | 0.14 / 0.23 | 0.58 / 2.06 | 0.49 / 1.15 | 158 ms |

Outlines and HUD, same frames:

| Method | Outlines f100 / f210 / f255 / f345 / f560 | HUD f100 / f210 / f255 / f345 / f560 |
|---|---|---|
| Ours (raw) | 2.08 / 3.80 / 0.43 / 6.00 / 4.51 | 0.00 / 0.06 / 0.05 / 0.33 / 0.02 |
| NeuFlow v2, 960x544 | 3.52 / 1.73 / 0.38 / 6.23 / 3.68 | 0.33 / 4.54 / 0.50 / 1.31 / 0.73 |
| NeuFlow v2, 1280x720 | 3.96 / 0.71 / 0.50 / 25.3 / 3.44 | 0.53 / 1.44 / 0.66 / 1.05 / 0.42 |
| RAFT large, 1920x1080 | 1.45 / 1.69 / 1.09 / 7.04 / 4.36 | 0.20 / 10.93 / 0.39 / 1.12 / 3.24 |

What this shows:

- **NeuFlow v2**, the only model fast enough to consider, is equal to or worse than the built-in
  estimator. It loses badly on objects moving in a still scene and on plain areas in the dolly. It is
  also less precise: on the fast pan 26% of textured pixels are within a quarter pixel, against 50%
  for ours.
- **NeuFlow v2 does not work above about 720p.** At 1920x1088 it returns errors of 20 to 150 px, in
  full precision as well, so it is the size and not half-float overflow. At 4K it ran out of memory.
- **RAFT large at 1080p** roughly halves our error on textured camera motion. It is twenty times too
  slow, worse than ours on moving objects, and worse on plain areas in the dolly.
- **Every network drags the HUD with the scene behind it** (1 to 11 px on the pan). Ours keeps it
  still. For a capture, where a HUD is always on screen, a network's output would need our static
  lock on top in any case.
- **DIS**, taken whole, is two to three times worse than ours. Borrowing single ideas from it (see
  "Shader-side leads") is a separate matter.

## Result 2: a model on top of our vectors

The idea: keep our vectors, and use the network only to catch and repair the wrong ones. Mean over
the five frames, textured / plain / outline, and the share of the moving scene wrong by more than 2 px.

| | Textured | Plain | Outline | Wrong by >2 px |
|---|---|---|---|---|
| Ours raw | 0.81 | 0.93 | 3.36 | 7.4% |
| + NeuFlow 720p, perfect chooser (ceiling, uses the truth) | 0.46 | 0.49 | 1.69 | 3.9% |
| + NeuFlow 720p, whichever reproduces the pixel better (7, 31 or 63 px window) | 0.80 | 0.92 | 3.36 | 7.3% |
| + NeuFlow 720p, the network's wherever they disagree by >2 px | 1.08 | 2.50 | 6.68 | 14.8% |
| + NeuFlow 720p, on disagreement the one closer to the neighbourhood | 1.03 | 1.36 | 3.86 | 13.5% |
| + NeuFlow 540p, perfect chooser | 0.52 | 0.57 | 1.77 | 5.1% |
| + NeuFlow 540p, the network's wherever they disagree | 1.14 | 1.94 | 2.98 | 21.7% |
| + RAFT large 1080p, perfect chooser | 0.29 | 0.36 | 1.20 | 2.3% |
| + RAFT large 1080p, the network's wherever they disagree | 0.50 | 0.91 | 3.10 | 5.7% |

Starting from the refined vectors (after the per-pixel stage: 1.01 / 1.18 / 3.52, 8.2%) the pattern
is the same.

Disagreement as a warning flag only (flag a pixel when the two vectors differ by more than 2 px):

| Model | Our wrong vectors flagged, f100 / f210 / f255 / f345 / f560 | Our good vectors flagged by mistake |
|---|---|---|
| NeuFlow v2, 960x544 | 41 / 73 / 94 / 87 / 78 % | 33.5 / 14.5 / 0.6 / 27.7 / 11.5 % |
| NeuFlow v2, 1280x720 | 40 / 74 / 89 / 82 / 74 % | 33.4 / 3.6 / 0.4 / 15.1 / 6.0 % |
| RAFT large, 1920x1080 | 90 / 89 / 92 / 88 / 85 % | 3.8 / 2.5 / 0.1 / 5.5 / 5.0 % |

What this shows:

- **The errors are complementary.** A chooser that always took the better of the two vectors would
  cut our error by about 45% with the small model and 60% with the large one.
- **No simple rule finds the better one.** Our wrong vectors pass every "does it reproduce the
  pixel" test, at any window size; that is why the estimator chose them. Taking the network's vector
  on disagreement makes things worse with the small model, because it is wrong more often than we are.
- **As a flag it is too noisy** with the small model: it marks most of our wrong vectors, and up to a
  third of the good ones.
- **Side finding.** The vectors we still get wrong are the ones that reproduce the pixel anyway
  (plain areas, along lines). That fits the rig's separate finding that more exact vectors did not
  improve the plain DLAA picture. Whether a neural consumer is more sensitive to them is untested.

## What a trained model would need

Only a model trained for this job could collect the gain the "perfect chooser" rows show. An outline,
as a proposal and not a measured plan:

- **Job**: take the two frames, our vectors, and depth when ReShade has it. Output a per-pixel choice
  or correction with a confidence, and a mask for flame, smoke and haze (no public method estimates
  motion for those; everyone relies on engine masks).
- **Size**: it has to work at a resolution where 4K detail survives. The small public model is
  limited to about 720p; ours would have to be trained at the size it runs at.
- **Training data**: a much richer generator than today's rig scene (many scenes, HUD styles,
  effects, capture artefacts), plus public sets with ground truth: Spring (4K flow, depth, camera
  poses, CC BY 4.0) and TartanAir (CC BY 4.0, static scenes only). One more source worth checking:
  games with native DLSS hand over true vectors and depth, which the feeder or OptiScaler can see.
  Untested.
- **Runtime**: an inference runtime in the add-on (ONNX Runtime with DirectML needs no per-machine
  build; TensorRT is faster and builds an engine per machine), a model file to ship, and a GPU
  budget next to DLSS. In PyTorch the small model costs 11 ms at 540p and 20 ms at 720p on an
  RTX 5090; an optimised runtime should do better, by an amount not measured here.
- **Effort**: weeks, mostly data and training, with no guarantee. DLSS itself is the proof that the
  hardware can run a far larger network in real time; the gap is training data and engineering.
- The per-pixel stage of the shader would still sit after it, and the HUD lock with it.

## Shader-side leads from the same survey

These need no network. Tags: [V] read at the source, [M] from memory.

- **V-shaped (equiangular) sub-pixel fit** instead of a parabola on a SAD cost: the parabola causes
  pixel locking (arXiv 2112.00941 [V], after Shimizu and Okutomi 2005).
- **One or two inverse-compositional Lucas-Kanade steps at the finest level**: where DIS says the
  small-displacement error goes (arXiv 1603.03590 [V]).
- **Rigid plus residual**: with depth, test the camera-fit vector as a candidate at every pixel and
  keep the free search only where the pixel is really dynamic (plane plus parallax, Irani and
  Anandan 1996 [M]; the static test in GFFE, arXiv 2406.18551 [V]).
- **Depth-clip disocclusion**: just-revealed pixels get the background vector (AMD FSR2/FSR3 docs [V]).
- **FSR3 Optical Flow** [V]: 8x8 blocks, exhaustive SAD, 3x3 vector median, integer vectors at 1/8
  resolution. Coarser than what the built-in estimator already does.
- **NVIDIA Optical Flow SDK**: a hardware block matcher. kibblerz/DLSS5-Reshade-AIO uses it at 180p
  with nearest-cell upscaling (read from its source, never run here). Not expected to beat a 2 px
  grid with sub-pixel fits, and it knows nothing about depth.

## Caveats

- One synthetic scene, five frames, no depth. A good or bad result here is not proof for real PS5
  capture.
- Pretrained weights only; no fine-tuning of any kind.
- Timings are PyTorch on a shared GPU.
- The picture (DLAA output) was not measured with any of these vectors, only vector error.
