# Stage the native plugin payload locally.
#
# This script does not write into the Kenshi installation. By default it places
# the release files beside this script.

param(
    [string]$Deps = "C:\Code\kenshilib-deps",
    [string]$Configuration = "Release",
    [string]$OutputDir = $PSScriptRoot,
    [switch]$SkipBuild,
    [switch]$ForceUnsupportedKenshiLib
)

$ErrorActionPreference = "Stop"

function Copy-IfDifferent {
    param(
        [string]$Source,
        [string]$Destination
    )

    $sourcePath = [System.IO.Path]::GetFullPath($Source)
    $destinationPath = [System.IO.Path]::GetFullPath($Destination)
    if ($sourcePath -ieq $destinationPath) { return }
    Copy-Item $Source $Destination -Force
}

if (-not $SkipBuild) {
    & (Join-Path $PSScriptRoot "build.ps1") `
        -Deps $Deps `
        -Configuration $Configuration `
        -ForceUnsupportedKenshiLib:$ForceUnsupportedKenshiLib
}

$dll = Join-Path $PSScriptRoot "TradersRefuse.dll"
$json = Join-Path $PSScriptRoot "TradersRefuse\RE_Kenshi.json"
$mod = Join-Path $PSScriptRoot "TradersRefuse\Traders Refuse Irrelevant Items.mod"

if (-not (Test-Path $dll)) {
    throw "Staged DLL not found: $dll. Run build.ps1 first, or run this script without -SkipBuild."
}
if (-not (Test-Path $json)) {
    throw "RE_Kenshi.json not found: $json."
}
if (-not (Test-Path $mod)) {
    throw "Kenshi mod file not found: $mod."
}

if (-not (Test-Path $OutputDir)) {
    New-Item -ItemType Directory -Path $OutputDir | Out-Null
}

Copy-IfDifferent $dll (Join-Path $OutputDir "TradersRefuse.dll")
Copy-IfDifferent $json (Join-Path $OutputDir "RE_Kenshi.json")
Copy-IfDifferent $mod (Join-Path $OutputDir "Traders Refuse Irrelevant Items.mod")

Write-Host "Staged plugin payload:"
Write-Host "  $(Join-Path $OutputDir 'TradersRefuse.dll')"
Write-Host "  $(Join-Path $OutputDir 'RE_Kenshi.json')"
Write-Host "  $(Join-Path $OutputDir 'Traders Refuse Irrelevant Items.mod')"
