$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$solution = Join-Path $root 'NexoraVanguard.sln'
$buildRoot = Join-Path $root 'build'
$objRoot = Join-Path $root 'obj'

function Resolve-MSBuild {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path -LiteralPath $vswhere) {
        $found = & $vswhere -latest -products * -requires Microsoft.Component.MSBuild -find 'MSBuild\**\Bin\MSBuild.exe' | Select-Object -First 1
        if ($found) {
            return $found
        }
    }

    return 'msbuild.exe'
}

Remove-Item -LiteralPath (Join-Path $buildRoot 'x86') -Recurse -Force -ErrorAction SilentlyContinue
Remove-Item -LiteralPath (Join-Path $buildRoot 'x64') -Recurse -Force -ErrorAction SilentlyContinue
Remove-Item -LiteralPath $objRoot -Recurse -Force -ErrorAction SilentlyContinue

New-Item -ItemType Directory -Path (Join-Path $buildRoot 'x86') | Out-Null
New-Item -ItemType Directory -Path (Join-Path $buildRoot 'x64') | Out-Null

$msbuild = Resolve-MSBuild

foreach ($platform in @('x86', 'x64')) {
    & $msbuild $solution /m /restore /t:Build /p:Configuration=Release /p:Platform=$platform
    if ($LASTEXITCODE -ne 0) {
        exit $LASTEXITCODE
    }
}

Get-ChildItem -LiteralPath $buildRoot -Recurse -File |
    Where-Object { $_.Name -ne 'NexoraVanguard.dll' } |
    Remove-Item -Force

Remove-Item -LiteralPath $objRoot -Recurse -Force -ErrorAction SilentlyContinue

Write-Host 'Build finalizado:'
Write-Host "  x86: $(Join-Path $buildRoot 'x86\NexoraVanguard.dll')"
Write-Host "  x64: $(Join-Path $buildRoot 'x64\NexoraVanguard.dll')"
