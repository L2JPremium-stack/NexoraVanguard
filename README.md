# Nexora Vanguard

Native Win32 companion DLL loaded by the Aion `version.dll` proxy.

## Features
- Tray icon using `assets\try_icon.ico`.
- Light and dark theme toggle with the moon button.
- Security requirement panel inspired by Vanguard-style status views.
- Tooltip text for each requirement row.
- Tray/menu commands for logs, precheck, theme change, and exit.
- Exit warning: leaving Nexora Vanguard while Aion is open closes the game process.

## Build
Run the helper batch file:

```bat
build-release.bat
```

Or run the PowerShell script directly:

```powershell
powershell -ExecutionPolicy Bypass -File .\build-release.ps1
```

Outputs:

```text
build\x86\NexoraVanguard.dll
build\x64\NexoraVanguard.dll
```

The build scripts clean intermediate files and keep only the compiled DLLs in
`build\x86` and `build\x64`.

## Client Limit
The simultaneous Aion client limit is controlled in code by:

```cpp
int InternalMaxGameClients() {
    // Internal build-time limit. Do not expose this through config, registry, or UI.
    return 0x5A ^ 0x5B;
}
```

`0x5A ^ 0x5B` is an XOR expression that evaluates to `1`, so the current build
allows only 1 game client per computer.

For a future build with 2 clients, change only that return value, for example:

```cpp
return 2;
```

If the dev wants to keep the same obfuscated style, `0x5A ^ 0x58` evaluates to
`2`. For 3 clients, use `return 3;` or an equivalent expression. Keep this as a
build-time code value only; do not expose it through config, registry, or UI.
