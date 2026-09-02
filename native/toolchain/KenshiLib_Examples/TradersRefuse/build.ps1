# Build TradersRefuse.dll.
#
# This helper expects the normal KenshiLib plugin toolchain described by the
# upstream KenshiLib Examples README: Release x64, Visual C++ v100, KenshiLib,
# and Boost 1.60.

param(
    [string]$Deps = "C:\Code\kenshilib-deps",
    [string]$Configuration = "Release",
    [switch]$ForceUnsupportedKenshiLib
)

$ErrorActionPreference = "Stop"
$VerifiedKenshiLibVersion = "0.4.0"

function Get-KenshiLibVersion {
    param([string]$DepsPath)

    $candidateFiles = @(
        (Join-Path $DepsPath "VERSION"),
        (Join-Path $DepsPath "version.txt"),
        (Join-Path $DepsPath "KenshiLib\VERSION"),
        (Join-Path $DepsPath "KenshiLib\version.txt"),
        (Join-Path $DepsPath "README.md"),
        (Join-Path $DepsPath "KenshiLib\README.md")
    )

    foreach ($file in $candidateFiles) {
        if (-not (Test-Path $file)) { continue }
        $match = Select-String -Path $file -Pattern "KenshiLib[^0-9]*(\d+\.\d+\.\d+)" -CaseSensitive:$false | Select-Object -First 1
        if ($match) { return $match.Matches[0].Groups[1].Value }

        $plainVersion = Select-String -Path $file -Pattern "^\s*(\d+\.\d+\.\d+)\s*$" | Select-Object -First 1
        if ($plainVersion) { return $plainVersion.Matches[0].Groups[1].Value }
    }

    if (Test-Path (Join-Path $DepsPath ".git")) {
        $tag = & git -C $DepsPath describe --tags --always --dirty 2>$null
        if ($LASTEXITCODE -eq 0 -and $tag -match "(\d+\.\d+\.\d+)") {
            return $Matches[1]
        }
    }

    return $null
}

if (-not (Test-Path (Join-Path $Deps "KenshiLib\Libraries\KenshiLib.lib"))) {
    throw "KenshiLib.lib was not found under '$Deps'. Set up KenshiLib_Examples_deps first, or pass -Deps."
}

$detectedVersion = Get-KenshiLibVersion -DepsPath $Deps
if ($detectedVersion) {
    Write-Host "KenshiLib $detectedVersion detected."
    if ($detectedVersion -ne $VerifiedKenshiLibVersion -and -not $ForceUnsupportedKenshiLib) {
        $answer = Read-Host "The mod is not verified to run on version $detectedVersion, proceed anyway? [y/N]"
        if ($answer -notmatch "^(y|yes)$") {
            throw "Build cancelled for unverified KenshiLib version $detectedVersion."
        }
    }
} else {
    Write-Warning "KenshiLib version could not be detected; continuing with the configured dependency path."
}

$env:KENSHILIB_DIR      = Join-Path $Deps "KenshiLib"
$env:KENSHILIB_DEPS_DIR = $Deps
$env:BOOST_INCLUDE_PATH = Join-Path $Deps "boost_1_60_0"
$env:BOOST_ROOT         = Join-Path $Deps "boost_1_60_0"

$vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) {
    throw "vswhere.exe was not found. Install Visual Studio with MSBuild."
}

$msbuild = & $vswhere -latest -products * -requires Microsoft.Component.MSBuild -find "MSBuild\**\Bin\MSBuild.exe" | Select-Object -First 1
if (-not $msbuild) {
    throw "MSBuild.exe was not found by vswhere."
}

$project = Join-Path $PSScriptRoot "TradersRefuse.vcxproj"
& $msbuild $project /t:Rebuild /p:Configuration=$Configuration /p:Platform=x64 /nologo /v:minimal
if ($LASTEXITCODE -ne 0) {
    throw "MSBuild failed with exit code $LASTEXITCODE."
}

$builtDll = Join-Path $PSScriptRoot "x64\$Configuration\TradersRefuse.dll"
$stagedDll = Join-Path $PSScriptRoot "TradersRefuse.dll"
Copy-Item $builtDll $stagedDll -Force

Write-Host "Built:  $builtDll"
Write-Host "Staged: $stagedDll"
