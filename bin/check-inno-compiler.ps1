# Probe the compiler itself: ISCC/ISCmplr PE version resources can be 0.0.0.0.
# Output=no performs syntax checking only; run in an isolated temp directory
# and disable output cleanup/manifests so no existing deployment is touched.
[CmdletBinding()]
param([string]$Compiler = $env:ISCC, [switch]$Quiet)
$ErrorActionPreference = 'Stop'
$temp = $null
$pushed = $false
$code = 1
try {
    if (!$Compiler) { throw 'No ISCC.exe path was supplied.' }
    $Compiler = (Resolve-Path -LiteralPath $Compiler.Trim('"')).Path
    $temp = Join-Path ([IO.Path]::GetTempPath()) ('omawrite-inno-check-' + [guid]::NewGuid())
    New-Item -ItemType Directory -Path $temp | Out-Null
    Push-Location $temp
    $pushed = $true
    $script = @'
[Setup]
AppName=Write compiler check
AppVersion=1
DefaultDirName={localappdata}\WriteCompilerCheck
PrivilegesRequired=lowest
Output=no
OutputDir=
OutputManifestFile=
Uninstallable=no
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
'@
    # PS 5.1 may promote redirected native stderr under Stop. Capture failures
    # as diagnostics and check the compiler exit status explicitly instead.
    # Native commands update the global automatic variable. A local assignment
    # shadows it when setup calls this script in-process, hiding successful exits.
    $global:LASTEXITCODE = 1
    $ErrorActionPreference = 'Continue'
    $output = $script | & $Compiler - 2>&1
    $compilerExit = $global:LASTEXITCODE
    $ErrorActionPreference = 'Stop'
    if ($compilerExit -ne 0) {
        throw "Compiler capability check failed for $Compiler. Inno Setup 6.3+ with x64compatible support is required.`n$($output -join "`n")"
    }
    if (!$Quiet) {
        Write-Host "Inno Setup compiler: $Compiler"
        $output | Where-Object { "$_" -like 'Compiler engine version:*' } | ForEach-Object { Write-Host "$_" }
        Write-Host 'Inno Setup 6.3+ compiler features verified; no installer was created or executed.'
    }
    $code = 0
} catch {
    if (!$Quiet) { Write-Host "ERROR: $_" }
} finally {
    if ($pushed) { Pop-Location }
    if ($temp -and (Test-Path -LiteralPath $temp)) { Remove-Item -LiteralPath $temp -Recurse -Force }
}
exit $code
