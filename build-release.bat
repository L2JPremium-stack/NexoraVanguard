@echo off
setlocal

pushd "%~dp0" || exit /b 1

powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0build-release.ps1"
set "BUILD_EXIT=%ERRORLEVEL%"

if not "%BUILD_EXIT%"=="0" (
    popd
    exit /b %BUILD_EXIT%
)

if exist "%~dp0obj" (
    rmdir /s /q "%~dp0obj"
)

if exist "%~dp0build" (
    for /r "%~dp0build" %%F in (*) do (
        if /i not "%%~nxF"=="NexoraVanguard.dll" (
            del /f /q "%%~fF" >nul 2>nul
        )
    )
)

echo.
echo Build finalizado. Arquivos mantidos:
if exist "%~dp0build\x86\NexoraVanguard.dll" echo   x86: "%~dp0build\x86\NexoraVanguard.dll"
if exist "%~dp0build\x64\NexoraVanguard.dll" echo   x64: "%~dp0build\x64\NexoraVanguard.dll"

popd
exit /b 0
