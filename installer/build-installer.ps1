<#
.SYNOPSIS
    Build the Archive Extractor installers (MSI + EXE bundle) with WiX.

.DESCRIPTION
    Produces, from an already-built ArchiveExtractor.exe + 7z.dll:
      * ArchiveExtractor.msi          — per-machine x64 MSI (Package.wxs)
      * ArchiveExtractorSetup.exe     — Burn bootstrapper wrapping the MSI (Bundle.wxs)

    Requires the .NET SDK (`dotnet`). The WiX CLI is installed as a global dotnet
    tool on demand, and the Bal bootstrapper extension is added if missing.

    Run a Release build first so build\x64-release has the exe and 7z.dll:
        .\scripts\build.ps1 -Release

.PARAMETER Version
    Product/bundle version (e.g. 1.2.3). Defaults to 0.1.0. CI passes the git tag.

.PARAMETER BuildDir
    Directory holding the built exe + 7z.dll. Defaults to build\x64-release.

.PARAMETER OutDir
    Where the .msi / setup .exe are written. Defaults to installer\out.

.EXAMPLE
    .\installer\build-installer.ps1 -Version 1.0.0
#>
[CmdletBinding()]
param(
    [string]$Version = "0.1.0",
    [string]$BuildDir,
    [string]$OutDir,
    # Pin WiX v5: the stable release that builds without the v6/v7 Open Source
    # Maintenance Fee (OSMF) EULA gate. The Bal extension is matched to it.
    [string]$WixVersion = "5.0.2"
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$RepoRoot   = Split-Path -Parent $PSScriptRoot
$InstallerDir = $PSScriptRoot
if (-not $BuildDir) { $BuildDir = Join-Path $RepoRoot 'build\x64-release' }
if (-not $OutDir)   { $OutDir   = Join-Path $InstallerDir 'out' }

$Exe = Join-Path $BuildDir 'ArchiveExtractor.exe'
$Dll = Join-Path $BuildDir '7z.dll'

function Write-Step([string]$m) { Write-Host "`n==> $m" -ForegroundColor Cyan }

# --- Preconditions ----------------------------------------------------------
if (-not (Test-Path $Exe)) {
    throw "ArchiveExtractor.exe not found at $Exe. Build it first: .\scripts\build.ps1 -Release"
}
if (-not (Test-Path $Dll)) {
    throw "7z.dll not found at $Dll. It is copied next to the exe by the build (needs 7-Zip installed)."
}
if (-not (Get-Command dotnet -ErrorAction SilentlyContinue)) {
    throw "dotnet (the .NET SDK) is required to install/run the WiX CLI."
}

New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

# --- WiX toolset ------------------------------------------------------------
Write-Step "Ensuring the WiX CLI ($WixVersion) is available"
# `tool update` installs the tool if missing and pins/downgrades to $WixVersion
# if a different version (e.g. a fee-gated v7) is already present.
dotnet tool update --global wix --version $WixVersion --allow-downgrade | Out-Host
if ($LASTEXITCODE -ne 0) { throw "Installing WiX $WixVersion failed (exit $LASTEXITCODE)." }
# The global tools dir may not be on PATH for this session; add it.
$toolsDir = Join-Path $env:USERPROFILE '.dotnet\tools'
if ((Test-Path (Join-Path $toolsDir 'wix.exe')) -and ($env:PATH -notlike "*$toolsDir*")) {
    $env:PATH = "$toolsDir;$env:PATH"
}
wix --version | Out-Host

# The standard Burn bootstrapper (bal:WixStandardBootstrapperApplication) ships
# in the BootstrapperApplications extension in WiX v5 (it was in Bal.wixext in v4).
$BootExt = "WixToolset.BootstrapperApplications.wixext"
Write-Step "Ensuring the WiX bootstrapper extension ($WixVersion) is added"
# Idempotent: re-adding the same version is a no-op. Pinned to match the CLI.
wix extension add -g "$BootExt/$WixVersion" | Out-Host
if ($LASTEXITCODE -ne 0) { throw "Adding $BootExt failed (exit $LASTEXITCODE)." }

# --- Build the MSI ----------------------------------------------------------
$Msi = Join-Path $OutDir 'ArchiveExtractor.msi'
Write-Step "Building MSI -> $Msi"
wix build (Join-Path $InstallerDir 'Package.wxs') `
    -arch x64 `
    -d "BuildDir=$BuildDir" `
    -d "Version=$Version" `
    -o $Msi
if ($LASTEXITCODE -ne 0) { throw "wix build (MSI) failed (exit $LASTEXITCODE)." }

# --- Build the EXE bundle ---------------------------------------------------
$Setup = Join-Path $OutDir 'ArchiveExtractorSetup.exe'
Write-Step "Building EXE bundle -> $Setup"
wix build (Join-Path $InstallerDir 'Bundle.wxs') `
    -ext $BootExt `
    -d "Version=$Version" `
    -d "MsiSource=$Msi" `
    -o $Setup
if ($LASTEXITCODE -ne 0) { throw "wix build (bundle) failed (exit $LASTEXITCODE)." }

Write-Host "`nDone." -ForegroundColor Green
Write-Host "  MSI   : $Msi"
Write-Host "  Setup : $Setup"
