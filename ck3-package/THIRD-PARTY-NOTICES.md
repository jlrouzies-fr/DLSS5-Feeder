# Third-party components and runtime acquisition

This package combines or acquires components from separate projects. They remain the property of
their respective authors and retain their own licenses and distribution terms.

- **ReShade** and **reshade-shaders** — <https://github.com/crosire/reshade> and
  <https://github.com/crosire/reshade-shaders>
- **LumeniteFX** — <https://github.com/umar-afzaal/LumeniteFX>
- **DLSS5-Feeder** — <https://github.com/jlrouzies-fr/DLSS5-Feeder>
- **RHI** — <https://github.com/RankFTW/RHI> (GPL-3.0; optional companion and runtime metadata source)
- **RenoDX DLSS 5 add-on** — community binary resolved through RHI's component repository; no
  redistribution permission is inferred from another RenoDX project's license
- **NVIDIA DLSS runtime** — downloaded from NVIDIA's official DLSS GitHub release under NVIDIA's
  RTX SDK license: <https://github.com/NVIDIA/DLSS/blob/main/LICENSE.txt>
- **NVIDIA DLSS Neural Rendering runtime** — selected through RHI's manifest; the Extended profile
  is a third-party modified, unsigned ShortFuse build and is not an official NVIDIA release

The default release is a thin bootstrap and does not contain NVIDIA, RenoDX, or ShortFuse runtime
binaries. Runtime acquisition is opt-in. `CK3-DLSS-RUNTIME.json` records the exact version, source
URL, signature status, signer, SHA-256 hash, and installation time for every active component.
`BUILD-PROVENANCE.txt` records build-time sources and hashes.

NVIDIA's SDK license places conditions on application integration and redistribution and prohibits
stand-alone or unauthorized distribution. RHI's GPL license covers RHI itself, not the separately
downloaded third-party runtimes. Building or downloading a package does not grant redistribution
rights; the publisher is responsible for all required notices and permissions.

