# Note to the DLSS5-Feeder developer

Hi Jean-Laurent,

Deep Fried Chicken 1.4.8 implements the Feeder compatibility surface you
requested. Thank you for the clear report and the offer to test it.

ABI-aware Feeder builds should no longer stop solely because Chicken's legacy
`NAME` export is present. Instead:

1. Read `DFC_FeederInteropAbi`; value `1` means negotiated coexistence is
   available.
2. Read `DFC_Feature1InterceptionState`. Continue when it is `CLAIMING` or
   `ARMED`; stand down when it is `DISARMED`, `CONFLICT`, or `FAILED`.
3. Immediately before feature-1 Create and every feature-1 Evaluate, publish
   these unsigned NGX parameters:

```cpp
parameters->Set("DFC.Feeder.ContractVersion", 1u);
parameters->Set("DFC.Feeder.ProviderId", 0x444C3546u);
parameters->Set("DFC.Feeder.HostMode", companion_x64 ? 1u : 0u);
parameters->Set("DFC.Feeder.EvaluateCadence", 1u);
```

Chicken binds the complete tuple to the successfully created handle and exact
hook host. Partial, unknown, or mismatched tuples fail closed: the genuine
Feeder call and output remain active while Chicken skips its own work. Chicken
reserves no more than one neural evaluation per Present.

The requested ownership courtesy is also implemented:

```text
Local\DeepFriedChicken.Feature1Consumer.v1.<decimal process ID>
```

This mutex is for feature-1 consumers. Feeder is the contract producer and
should not claim it. `arm=0` is a restart-only hard disarm that claims no
consumer marker and installs no native-NGX, Streamline, loader, or resolver
interception. The legacy `NAME` export remains stable for older mutual-
exclusion checks, while ABI-aware builds should prefer the two explicit data
exports above.

Feeder's add-on and x64 companion host are explicitly treated as compatible
transports. Their loader imports are exempt from Chicken patching, and resolver
observation retains the genuine NGX target rather than substituting a Chicken
wrapper as the address returned to Feeder. Tracking is allocation-bounded,
Feeder's feature-1 call is always forwarded, and Chicken retires only its own
feature-18 graph/codecs/intermediates after D3D12 fence proof. Chicken never
releases Feeder's feature-1 handle.

## Suggested first test

- Keep `dlss5-feed`; remove `renodx-dlss5.addon64` and
  `renodx-dlss.addon64` because Chicken replaces that neural consumer.
- Do not combine Feeder with `dlss5-dx11-bridge.addon64`.
- Set Chicken `arm=1`, Feeder `warmup_rebuild=0`, one Chicken pass, 100%
  Feeder work resolution, Texture Boost off, then fully restart.
- Disable Smooth Motion and OptiScaler while isolating the interop path.
- Test both `HostMode=0` in-process and `HostMode=1` in a version-matched x64
  companion.
- Exercise repeated feature recreation, alternating extents, SDR/HDR carriers,
  device recreation, partial markers, unknown values, and Create/Evaluate
  marker mismatch.
- Send `deep-fried-chicken.log`, `dlss5-feed.log`, game `ReShade.log`, the
  companion `host64\ReShade.log` where applicable, and the motion-vector/depth
  probe lines after at least 600 frames.

## Current boundary

Chicken 1.4.8 is source-contract compatible with Feeder v0.7.0 and the current
v0.8.0-beta.3 source reference. Older exact upstream identities without the
new keys retain a narrow legacy fallback. Cross-API community validation is
still required: the 32-bit Vulkan route remains experimental upstream,
64-bit OpenGL coverage is limited, and D3D9 depends on translation. The
current 32-bit host does not publish the trust mask, so Chicken does not invent
one.

Feeder's reduced work-resolution option remains a scaled synthetic DLAA
contract rather than genuine DLSS Performance/Balanced/Quality modes. Frame
Generation remains game-owned and is forwarded. Chicken 1.4.7 added a
default-off same-producer-list base-frame copyback experiment which never cooks
generated frames; it has local native-DLSS evidence but is not yet claimed as
validated on a Feeder transport.

Version 1.4.8 also replaces Chicken's recurring 300-Present full-module scan
with loader/resolver and `LdrRegisterDllNotification` signals when OS
notification registration succeeds. If registration is unavailable, bootstrap
scanning remains until neural proof and then slows to a 3600-Present safety
fallback. Manual Refresh requests discovery. This directly addresses the
periodic CPU-work candidate you identified, though an A/B on the affected host
is still needed to confirm the 1% low improvement.

The stable producer contract is in `FEEDER-INTEROP-v1.md`; full layouts,
fallback behavior, transport matrix, and logs are in
`FEEDER-COMPATIBILITY.md`.
