# Third-party components and runtime acquisition

This package combines or acquires components from separate projects. They remain the property of
their respective authors and retain their own licenses and distribution terms.

- **ReShade** and **reshade-shaders** — <https://github.com/crosire/reshade> and
  <https://github.com/crosire/reshade-shaders>
- **Lilium HDR Shaders 2026.02.28** — <https://github.com/EndlesslyFlowering/ReShade_HDR_shaders>
  (GPL-3.0; the complete shader source, textures, and license are bundled unchanged)
- **LumeniteFX** — <https://github.com/umar-afzaal/LumeniteFX>
- **DLSS5-Feeder** — <https://github.com/jlrouzies-fr/DLSS5-Feeder>
- **RHI** — <https://github.com/RankFTW/RHI> (GPL-3.0; optional companion and runtime metadata source)
- **RenoDX DLSS 5 add-on** — community binary resolved through RHI's component repository; no
  redistribution permission is inferred from another RenoDX project's license
- **NVIDIA DLSS runtime** — downloaded from NVIDIA's official DLSS GitHub release under NVIDIA's
  RTX SDK license: <https://github.com/NVIDIA/DLSS/blob/main/LICENSE.txt>
- **NVIDIA DLSS Neural Rendering runtime** — selected through RHI's manifest; the Extended profile
  is a third-party modified, unsigned ShortFuse build and is not an official NVIDIA release

- **NVIDIA Streamline SDK headers** - used to build the optional native Vulkan bootstrap under the
  license reproduced as `NVIDIA-Streamline-LICENSE.txt`; <https://github.com/NVIDIA-RTX/Streamline>
- **NVIDIA Streamline runtime plugins** - optional donor-supplied, NVIDIA-signed binaries staged only
  into acknowledged private/offline builds; their inclusion does not grant redistribution permission
- **Kotlin 2.4.0** - application language and standard library; Apache License 2.0;
  <https://github.com/JetBrains/kotlin>
- **Compose Multiplatform 1.12.0** - desktop user-interface runtime; Apache License 2.0;
  <https://github.com/JetBrains/compose-multiplatform>
- **Skiko 0.150.1** - Compose Desktop graphics runtime; Apache License 2.0;
  <https://github.com/JetBrains/skiko>

The Apache License 2.0 text covering the Kotlin installer dependencies is reproduced as
`THIRD-PARTY-LICENSES\Apache-2.0.txt`.

The default release is a thin bootstrap and does not contain NVIDIA, RenoDX, or ShortFuse runtime
binaries. Runtime acquisition is opt-in. `CK3-DLSS-RUNTIME.json` records the exact version, source
URL, signature status, signer, SHA-256 hash, and installation time for every active component.
`BUILD-PROVENANCE.txt` records build-time sources and hashes.

NVIDIA's SDK license places conditions on application integration and redistribution and prohibits
stand-alone or unauthorized distribution. RHI's GPL license covers RHI itself, not the separately
downloaded third-party runtimes. Building or downloading a package does not grant redistribution
rights; the publisher is responsible for all required notices and permissions.

