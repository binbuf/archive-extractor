<#
.SYNOPSIS
    Local dev build / test / install / uninstall helper for ArchiveExtractor.

.DESCRIPTION
    Sets up the VS x64 toolchain (via scripts\dev-env.cmd), then configures,
    builds, and runs the test suite for a local development build.

    Default (no flags) : build the DEBUG config and run its tests.
    -Release           : build the RELEASE config (and run its tests) instead.
    -Clean             : rebuild this config's project from scratch (preserving the
                         prebuilt vcpkg dependencies; see note below).
    -Install           : after a green build, register (elevated) the file-type
                         associations to point at THIS local build's exe, so
                         double-clicking an archive launches it. Re-run after any
                         build to repoint Windows at the latest exe.
    -Uninstall         : run the built exe's --unregister (elevated) to remove the
                         file-type association registry entries, then exit. Does not
                         build or test. Combine with -Release to pick the release exe.

    Dependency note: a from-scratch "vcpkg install" currently fails on this machine
    because the VS-bundled CMake (4.2) dropped support for the old
    cmake_minimum_required that libarchive 3.7.7 declares. The dependencies are
    therefore treated as a prebuilt, stable artifact: -Clean keeps each config's
    build\<cfg>\vcpkg_installed tree, and a fresh config is seeded from a sibling
    config's tree. vcpkg's per-configure install is a no-op as long as that tree is
    intact, so normal builds Just Work. (If you ever need to add a dependency,
    build the vcpkg_installed tree out-of-band — e.g. with a CMake < 4.0.)

.EXAMPLE
    .\scripts\build.ps1                 # debug build + tests
    .\scripts\build.ps1 -Release        # release build + tests
    .\scripts\build.ps1 -Clean          # clean debug rebuild (keeps deps) + tests
    .\scripts\build.ps1 -Release -Clean # clean release rebuild (keeps deps) + tests
    .\scripts\build.ps1 -Install        # build debug, then register it (elevated)
    .\scripts\build.ps1 -Release -Install   # build release, then register it (elevated)
    .\scripts\build.ps1 -Uninstall      # remove file associations (elevated)
#>
[CmdletBinding()]
param(
    [switch]$Release,
    [switch]$Clean,
    [switch]$Install,
    [switch]$Uninstall
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

if ($Install -and $Uninstall) {
    throw "-Install and -Uninstall are mutually exclusive; pick one."
}

# --- Paths & config --------------------------------------------------------
$RepoRoot = Split-Path -Parent $PSScriptRoot
$Config   = if ($Release) { 'x64-release' } else { 'x64-debug' }
$BuildDir = Join-Path $RepoRoot "build\$Config"
$Exe      = Join-Path $BuildDir 'ArchiveExtractor.exe'

# A marker file that indicates a complete vcpkg dependency tree for the triplet.
$DepMarker = 'vcpkg_installed\x64-windows-static\lib\archive.lib'

function Write-Step([string]$Message) {
    Write-Host ""
    Write-Host "==> $Message" -ForegroundColor Cyan
}

function Assert-LastExit([string]$What) {
    if ($LASTEXITCODE -ne 0) { throw "$What failed (exit $LASTEXITCODE)." }
}

# Run the app with a registration argument elevated (HKLM needs admin). Launches
# just the exe with a UAC prompt rather than re-running this whole script as
# admin, and surfaces the exe's exit code.
function Invoke-RegistrationVerb([string]$exe, [string]$argument, [string]$label) {
    Write-Step "$label via: $exe $argument (elevated)"
    $proc = Start-Process -FilePath $exe -ArgumentList $argument -Verb RunAs -Wait -PassThru
    if ($proc.ExitCode -ne 0) {
        throw "$argument failed (exit $($proc.ExitCode)). (Did you approve the elevation prompt?)"
    }
}

# Ensure $BuildDir has a complete prebuilt vcpkg_installed tree. If it doesn't,
# seed it from the sibling config's build dir (robocopy handles the deep/long
# dependency paths that Copy-Item chokes on). Errors clearly if no prebuilt tree
# exists anywhere, since a fresh install can't be produced in this environment.
function Initialize-Dependencies {
    if (Test-Path (Join-Path $BuildDir $DepMarker)) { return }

    $sibling = if ($Release) { 'x64-debug' } else { 'x64-release' }
    $srcTree = Join-Path $RepoRoot "build\$sibling\vcpkg_installed"
    if (-not (Test-Path (Join-Path $RepoRoot "build\$sibling\$DepMarker"))) {
        throw @"
No prebuilt vcpkg dependencies found for '$Config', and none to seed from in '$sibling'.
A from-scratch 'vcpkg install' fails here (VS-bundled CMake 4.2 vs libarchive's old
cmake_minimum_required). Build the dependency tree out-of-band (CMake < 4.0) so that
build\<cfg>\vcpkg_installed exists, then re-run this script.
"@
    }

    Write-Step "Seeding dependencies for $Config from $sibling (vcpkg_installed)"
    $dstTree = Join-Path $BuildDir 'vcpkg_installed'
    if (-not (Test-Path $BuildDir)) { New-Item -ItemType Directory -Path $BuildDir | Out-Null }
    robocopy $srcTree $dstTree /E /MT:16 /NFL /NDL /NJH /NJS /NP /R:1 /W:1 | Out-Null
    $global:LASTEXITCODE = 0  # robocopy uses 0-7 for success; don't trip Assert-LastExit
    if (-not (Test-Path (Join-Path $BuildDir $DepMarker))) {
        throw "Failed to seed dependencies into $dstTree."
    }
}

# --- -Uninstall: remove the file-association registry (elevated) -----------
# Standalone maintenance action: it does not build or test. The app writes its
# associations under HKLM, so --unregister needs elevation; we launch just the
# exe elevated (UAC) rather than re-running this whole script as admin.
if ($Uninstall) {
    $exeToUse = $Exe
    if (-not (Test-Path $exeToUse)) {
        $otherConfig = if ($Release) { 'x64-debug' } else { 'x64-release' }
        $otherExe = Join-Path $RepoRoot "build\$otherConfig\ArchiveExtractor.exe"
        if (Test-Path $otherExe) {
            $exeToUse = $otherExe
        } else {
            throw "ArchiveExtractor.exe not found (looked in build\x64-debug and build\x64-release). Build it first: .\scripts\build.ps1"
        }
    }

    Invoke-RegistrationVerb $exeToUse '--unregister' 'Removing file associations'
    Write-Host "File associations removed." -ForegroundColor Green
    return
}

# --- Toolchain environment -------------------------------------------------
# Reuse the canonical setup in dev-env.cmd (sources vcvars64 + adds CMake/Ninja
# to PATH and sets VCPKG_ROOT), importing the resulting environment into this
# PowerShell session so cmake/ctest run as native commands with clean output and
# real exit codes.
Write-Step "Setting up the VS x64 toolchain"
$DevEnv = Join-Path $PSScriptRoot 'dev-env.cmd'
if (-not (Test-Path $DevEnv)) { throw "dev-env.cmd not found at $DevEnv." }
$envDump = cmd /c "call `"$DevEnv`" set" 2>$null
foreach ($line in $envDump) {
    if ($line -match '^([^=]+)=(.*)$') { Set-Item -Path "Env:$($matches[1])" -Value $matches[2] }
}
Write-Host "cmake = $((Get-Command cmake).Source)"

# --- -Clean: rebuild the project from scratch, but keep the prebuilt deps ---
# Deleting the whole build dir would also drop vcpkg_installed and force a fresh
# (currently-broken) vcpkg install. Instead we wipe everything EXCEPT
# vcpkg_installed, so the configure's vcpkg step stays a no-op while all of our
# own CMake cache + object files are rebuilt.
if ($Clean -and (Test-Path $BuildDir)) {
    Write-Step "Clean: removing build artifacts in $BuildDir (keeping vcpkg_installed)"
    Get-ChildItem -LiteralPath $BuildDir -Force |
        Where-Object { $_.Name -ne 'vcpkg_installed' } |
        Remove-Item -Recurse -Force
}

# Make sure the dependency tree is present (seed from a sibling config if needed).
Initialize-Dependencies

# --- Configure (only when there's no cache; build auto-reconfigures otherwise)
if (-not (Test-Path (Join-Path $BuildDir 'CMakeCache.txt'))) {
    Write-Step "Configuring ($Config)"
    cmake --preset $Config
    Assert-LastExit 'cmake configure'
}

# --- Build -----------------------------------------------------------------
Write-Step "Building ($Config)"
cmake --build --preset $Config
Assert-LastExit 'cmake build'

# --- Test ------------------------------------------------------------------
Write-Step "Running tests ($Config)"
ctest --preset $Config --output-on-failure
Assert-LastExit 'ctest'

Write-Host ""
Write-Host "Done: $Config build green." -ForegroundColor Green
Write-Host "  Exe: $Exe"

# --- -Install: point the file associations at this local build (elevated) --
# registration.cpp writes shell\open\command / DefaultIcon from the registering
# process's own path, so registering THIS exe makes double-click launch the
# local build. Re-run -Install after any rebuild to repoint at the latest exe.
if ($Install) {
    Invoke-RegistrationVerb $Exe '--register' 'Registering local build for file associations'
    Write-Host ""
    Write-Host "Installed: archive associations now point at this build." -ForegroundColor Green
    $relFlag = if ($Release) { ' -Release' } else { '' }
    Write-Host "  Test     : double-click a supported archive, or run  $Exe <archive>"
    Write-Host "  Owned ext: for already-claimed types like .zip, set the default via"
    Write-Host "             $Exe --set-default <file>  (opens the Windows picker)"
    Write-Host "  Remove   : .\scripts\build.ps1 -Uninstall$relFlag"
}
