# Builds the downloadable zip of the side-by-side 3D fork from the Release build (run build-release.bat first).
param(
	[string]$BuildDir = "$env:LOCALAPPDATA\devilutionx-sbs\build-release",
	[string]$OutDir = "$PSScriptRoot\..\..\dist"
)
$ErrorActionPreference = 'Stop'
$root = Resolve-Path "$PSScriptRoot\..\.."
$build = Resolve-Path $BuildDir

$version = (Get-Content "$root\VERSION" -Raw).Trim()
$commit = (git -C $root rev-parse --short HEAD).Trim()
$name = "devilutionx-sbs-$version-$commit-windows-x64"
$stage = Join-Path $OutDir $name

if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory -Force $stage | Out-Null

Copy-Item "$build\devilutionx.exe" $stage
Copy-Item "$build\assets" $stage -Recurse
Copy-Item "$build\mods" $stage -Recurse
Copy-Item "$root\STEREO-SBS.md", "$root\LICENSE.md" $stage
Copy-Item "$PSScriptRoot\sbs-README.txt" (Join-Path $stage 'README.txt')

$zip = Join-Path $OutDir "$name.zip"
if (Test-Path $zip) { Remove-Item $zip -Force }
Compress-Archive -Path "$stage\*" -DestinationPath $zip
Write-Output $zip
