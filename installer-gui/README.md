# CK3 DLSS Installer GUI

Compose Desktop front end for the existing `DLSS5-CK3.ps1` installer. The GUI does not duplicate
installation logic: it validates the selected CK3 folder and invokes the tested PowerShell entry point. The compact card layout takes desktop-first visual cues from Aurora without adding Aurora's command/projection framework as a runtime dependency.

## Development

```powershell
./gradlew.bat run
./gradlew.bat test
./gradlew.bat createReleaseDistributable
```

Set `CK3_DLSS_PACKAGE_ROOT` when running from the IDE if the package root cannot be discovered
automatically. The repository layout is detected without the environment variable.

Use `../Build-Installer-GUI.ps1` to build the self-contained Windows application image and stage it
under `ck3-package/tools/CK3-DLSS-Installer`. The native executable uses `../resources/icon.ico`;
`src/main/resources/icon.png` is generated from the same artwork for the Compose window.
