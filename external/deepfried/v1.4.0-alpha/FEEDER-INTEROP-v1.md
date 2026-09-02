# Deep Fried Chicken / contract-feeder interop ABI 1

This document defines the small, public compatibility surface between Deep
Fried Chicken (DFC) and a producer of synthetic NGX feature-1 contracts. It is
an interoperability protocol, not a dependency: neither project loads or
embeds the other.

The constants are also provided in `dfc-feeder-interop.h`. The exact names and
numeric values in that header are stable for ABI 1.

## DFC discovery and ownership

DFC keeps its ordinary ReShade `NAME` data export beginning with `Deep Fried
Chicken ` as a stable legacy detection surface. ABI-aware producers should
prefer these exported data symbols:

```text
DFC_FeederInteropAbi              unsigned int; value 1
DFC_Feature1InterceptionState     LONG; DfcFeature1InterceptionState
```

`CLAIMING` and `ARMED` mean that DFC is available as the feature-1 neural
consumer. `DISARMED`, `CONFLICT`, and `FAILED` mean it is not available for
new synthetic work. A producer must not write either export.

DFC uses the process-local mutex below as a first-comer courtesy between
feature-1 *consumers*:

```text
Local\DeepFriedChicken.Feature1Consumer.v1.<decimal process ID>
```

A feeder is a contract producer, not a neural consumer, and must not acquire
this mutex. A cooperating competing consumer may acquire it before installing
its hooks; if it already exists, that consumer should remain inert.

The main config key `arm=0` is a restart-only hard disarm. It prevents DFC from
claiming the consumer marker and from installing native-NGX, Streamline, or
loader/resolver interception. `enabled=0` is different: it turns neural work
off live, but an already armed process keeps its permanent observation hooks.

## Marking a synthetic contract

Set all four unsigned-integer keys on the shared NGX parameter object before
*both* feature-1 Create and every feature-1 Evaluate:

| Key | ABI 1 value |
| --- | --- |
| `DFC.Feeder.ContractVersion` | `1` |
| `DFC.Feeder.ProviderId` | `0x444C3546` (`DL5F`) |
| `DFC.Feeder.HostMode` | `0` in-process, `1` companion x64 host |
| `DFC.Feeder.EvaluateCadence` | `1` (one Evaluate per Present) |

Minimal producer-side helper:

```cpp
static void PublishDfcInterop(NVSDK_NGX_Parameter *parameters,
                              bool companion_x64)
{
    if (parameters == nullptr) return;
    parameters->Set("DFC.Feeder.ContractVersion", 1u);
    parameters->Set("DFC.Feeder.ProviderId", 0x444C3546u);
    parameters->Set("DFC.Feeder.HostMode", companion_x64 ? 1u : 0u);
    parameters->Set("DFC.Feeder.EvaluateCadence", 1u);
}
```

Call this immediately before the existing feature-1 Create helper and again
immediately before each Evaluate helper. Unknown NGX keys are intentionally
ordinary application parameters; an older consumer ignores them.

DFC classifies the marker atomically:

- no keys: ordinary native contract;
- all four known values: supported Feeder contract;
- a partial tuple or an unknown version/provider/host/cadence: invalid marker,
  with no Feeder-only exception granted.

The explicit marker is tied to the created handle and hook-host provenance.
An Evaluate cannot turn an unrelated native handle into a Feeder handle, and a
marked Create must receive the same complete tuple at Evaluate.

For an older Feeder that does not yet publish ABI 1, DFC can retain a narrowly
scoped legacy fallback for the exact upstream add-on/host identities. That
fallback is compatibility debt and must not be used to identify arbitrary
renamed modules.

## Lifetime and call-boundary rules

- The feeder guarantees at most one synthetic feature-1 Evaluate per Present.
- A resolution change, history reset, warm-up rebuild, or host rebuild may
  release and recreate feature 1 normally.
- DFC never releases the feeder's feature-1 handle. It fences submitted DFC
  work, releases only its owned feature-18 graph, codecs, and intermediates,
  then reuses bounded slots for the replacement contract.
- DFC calls the genuine Create/Evaluate/Release target selected for the exact
  hook host. Resolver observation never substitutes a DFC wrapper as the
  address returned to a feeder.
- DFC forwards feature 11 (Frame Generation), feature 13 (Ray Reconstruction),
  and unknown NGX features without adopting them.
- Public calls keep their public Streamline command-list object. DFC may query
  the private native command-list interface only for its own codec/feature-18
  work and balances that private reference before returning.

If a fence, native release, target revalidation, or ownership check is
uncertain, DFC fails closed. It leaves the producer's native output intact and
retains possibly in-flight DFC objects until a safe boundary or process exit.

## 32-bit host path

A 32-bit game cannot load the 64-bit DFC add-on. For Feeder's companion path,
DFC must be installed in the x64 host's ReShade environment. The host publishes
`DFC.Feeder.HostMode=1`; the game-side x86 add-on does not attempt to negotiate
directly with DFC. Host executable, x86 add-on, IPC protocol, shaders, and
configuration must remain version matched.

## Suggested compatibility test matrix

Test the same ABI build with one pass first, then multiple passes:

- 64-bit D3D12 same-device;
- 64-bit D3D11 shared D3D12 transport;
- 64-bit Vulkan and OpenGL shared-resource routes;
- 32-bit D3D11/OpenGL companion host;
- experimental 32-bit Vulkan companion route;
- at least 100 successful feature release/recreate cycles at fixed extent;
- alternating output extents, HDR/SDR carriers, and device recreation;
- DFC `arm=0`, duplicate consumer, partial marker, unknown ABI, and mismatched
  Create/Evaluate marker negative cases.

For every failure, collect the DFC, Feeder, ReShade, and companion-host logs
from the same run.
