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
Run:

```powershell
powershell -ExecutionPolicy Bypass -File .\build-release.ps1
```

Outputs:

```text
build\x86\NexoraVanguard.dll
build\x64\NexoraVanguard.dll
```
