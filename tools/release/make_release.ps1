# Builds the mod and packs a release zip (no game files, no d3d11_orig.dll).
# Usage: .\tools\release\make_release.ps1 [-Version 1.0] [-OutDir <folder>]
param([string]$Version = '1.0', [string]$OutDir = (Join-Path $PSScriptRoot '..\..\build'))

$root = Resolve-Path (Join-Path $PSScriptRoot '..\..')
Set-Location $root

# Build into a scratch game folder so no real install is touched.
$scratch = Join-Path $env:TEMP "flower_vr_release_build"
New-Item -ItemType Directory -Force (Join-Path $scratch 'Data\Scripts') | Out-Null
$env:GAMEDIR = $scratch
cmd.exe /c "$root\build.bat"
Remove-Item Env:GAMEDIR
if (-not (Test-Path "$root\build\d3d11.dll")) { throw 'build failed' }

$stage = Join-Path $env:TEMP "flower_vr_release_stage"
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
New-Item -ItemType Directory $stage | Out-Null
Copy-Item "$root\build\d3d11.dll" $stage
Copy-Item "$root\tools\release\install.bat" $stage
Copy-Item "$root\tools\release\INSTALL.txt" $stage
Copy-Item "$root\tools\make_overrides.ps1" (Join-Path $stage 'vrmod_make_overrides.ps1')
Copy-Item "$root\vrmod.ini.example" $stage
Copy-Item "$root\vrmod_Flower.cfg.example" $stage
Copy-Item "$root\LICENSE" (Join-Path $stage 'LICENSE.txt')
Copy-Item "$root\THIRD_PARTY_NOTICES.md" $stage
Copy-Item "$root\thirdparty\minhook\LICENSE.txt" (Join-Path $stage 'MinHook-LICENSE.txt')

New-Item -ItemType Directory -Force $OutDir | Out-Null
$zip = Join-Path (Resolve-Path $OutDir) "FlowerVR-$Version.zip"
if (Test-Path $zip) { Remove-Item $zip }
Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zip
Get-ChildItem $stage | Select-Object Name, Length | Format-Table -AutoSize
"release: $zip ($([math]::Round((Get-Item $zip).Length / 1KB)) KB)"
