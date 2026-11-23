[CmdletBinding()]
param([Parameter(Mandatory)][string]$DistDir, [Parameter(Mandatory)][string]$Iscc,
    [Parameter(Mandatory)][ValidateSet('win32-msvc', 'win32-g++')][string]$Kit)
$ErrorActionPreference = 'Stop'
try {
    $DistDir = (Resolve-Path -LiteralPath $DistDir).Path
    $exe = Join-Path $DistDir 'omawrite.exe'
    if (!(Test-Path -LiteralPath $exe) -or !(Test-Path -LiteralPath "$DistDir\notices\COPYRIGHT.md")) {
        throw 'A complete build.bat --deploy folder with notices is required.'
    }
    # A GUI-subsystem EXE at the end of a PowerShell pipeline is asynchronous,
    # even in an assignment. A downstream cmdlet forces waiting/output capture.
    $global:LASTEXITCODE = 1
    $ErrorActionPreference = 'Continue'
    $output = & $exe --version 2>&1 | Out-String
    $versionExit = $global:LASTEXITCODE
    $ErrorActionPreference = 'Stop'
    if ($versionExit -ne 0) {
        $hex = '{0:X8}' -f ($versionExit -band 0xffffffffL)
        throw "Could not read the deployed application version: $exe (exit $versionExit / 0x$hex). Check the deployed Qt/compiler runtime DLLs.`n$output"
    }
    $match = [regex]::Match($output, '(?im)^omawrite\s+(\d+\.\d+\.\d+)\s*$')
    if (!$match.Success) { throw "Unexpected --version output: $output" }
    $version = $match.Groups[1].Value
    $destination = Join-Path (Split-Path $DistDir -Parent) 'installer'
    $artifact = Join-Path $destination "Write-$version-windows-x64-setup.exe"
    if (Test-Path -LiteralPath $artifact) { throw "Installer already exists; move it first: $artifact" }
    $needsVCRuntime = if ($Kit -eq 'win32-msvc') { 1 } else { 0 }
    & $Iscc "/DNeedsVCRuntime=$needsVCRuntime" "/DDistDir=$DistDir" "/DAppVersion=$version" "/DOutputDir=$destination" (Join-Path (Split-Path $PSScriptRoot -Parent) 'packaging\windows.iss')
    if ($LASTEXITCODE -ne 0 -or !(Test-Path -LiteralPath $artifact)) { throw 'Inno Setup compilation failed.' }
    Write-Host "Unsigned installer: $artifact"
    Write-Host 'Requires VC++ x64 Redistributable on MSVC targets. Node/Mermaid are separate. Not redistribution-cleared.'
} catch { Write-Error $_; exit 1 }
