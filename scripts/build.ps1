<#
.SYNOPSIS
    Build (and optionally flash) PgrOS for the LilyGo T-LoRa Pager.

.DESCRIPTION
    PgrOS is an overlay on a pinned Meshtastic firmware checkout. The PlatformIO
    project root is vendor/firmware; PgrOS sources are pulled in from ../../src/pgros
    by the [env:pgros] section in pgros.ini, which vendor/firmware/platformio.ini
    includes via a small integration patch.

    This wrapper exists so nobody has to remember the -d flag or to apply patches
    first.

.PARAMETER Target
    build (default), upload, clean, monitor, or fs (build+upload the LittleFS image).

.PARAMETER Port
    Serial port for upload/monitor, e.g. COM7. Auto-detected if omitted.

.EXAMPLE
    .\scripts\build.ps1
    .\scripts\build.ps1 -Target upload -Port COM7
#>
[CmdletBinding()]
param(
    [ValidateSet('build', 'upload', 'clean', 'monitor', 'fs')]
    [string]$Target = 'build',

    [string]$Port
)

$ErrorActionPreference = 'Stop'

$Root = Split-Path -Parent $PSScriptRoot
$Vendor = Join-Path $Root 'vendor\firmware'
$Env = 'pgros'

if (-not (Test-Path (Join-Path $Vendor '.git'))) {
    Write-Error "vendor/firmware is missing. Run: git submodule update --init --recursive"
}

# Locate the PlatformIO CLI. It is normally installed under the user profile by
# the VS Code extension rather than being on PATH.
$Pio = Get-Command pio -ErrorAction SilentlyContinue
if ($null -eq $Pio) {
    $Candidate = Join-Path $HOME '.platformio\penv\Scripts\pio.exe'
    if (Test-Path $Candidate) {
        $Pio = $Candidate
    }
    else {
        Write-Error "PlatformIO CLI not found. Install it, or check ~/.platformio/penv/Scripts/pio.exe"
    }
}
else {
    $Pio = $Pio.Source
}

# Patches must be in place before any compile: one of them is what teaches
# vendor/firmware/platformio.ini about the pgros env in the first place.
Write-Host "==> Applying patch set" -ForegroundColor Cyan
$Bash = Get-Command bash -ErrorAction SilentlyContinue
if ($null -ne $Bash) {
    & $Bash.Source (Join-Path $PSScriptRoot 'apply-patches.sh')
    if ($LASTEXITCODE -ne 0) { Write-Error "Patch application failed." }
}
else {
    Write-Warning "bash not found; skipping patch step. Apply patches/ manually."
}

$PioArgs = @()
switch ($Target) {
    'build' { $PioArgs = @('run', '-d', $Vendor, '-e', $Env) }
    'clean' { $PioArgs = @('run', '-d', $Vendor, '-e', $Env, '-t', 'clean') }
    'upload' { $PioArgs = @('run', '-d', $Vendor, '-e', $Env, '-t', 'upload') }
    'fs' { $PioArgs = @('run', '-d', $Vendor, '-e', $Env, '-t', 'uploadfs') }
    'monitor' { $PioArgs = @('device', 'monitor', '-d', $Vendor, '-e', $Env) }
}

if ($Port) {
    $PioArgs += @('--upload-port', $Port)
}

Write-Host "==> pio $($PioArgs -join ' ')" -ForegroundColor Cyan
& $Pio @PioArgs
exit $LASTEXITCODE
