This community fork was created to help players get DLSS 5 neural rendering in D3D12 games that do not natively support DLSS.

It is primarily aimed at D3D12 games such as WWE 2K26, where the game does not provide native DLSS support.

D3D12 Work Resolution allows the neural rendering workload to run at a lower internal work resolution, reducing the processing cost. The resulting image is then expanded back to the game's native backbuffer resolution using FSR 1 EASU + RCAS. This is a performance/cost control, not DLSS Super Resolution or DLSS Quality/Balanced/Performance.

D3D12 Sharpness adds a dedicated RCAS sharpening pass after the expand-back stage, allowing the player to control the final image sharpness independently.

The goal is to provide players with an additional option for D3D12 games that do not already offer DLSS, while keeping the implementation separate and modular.
