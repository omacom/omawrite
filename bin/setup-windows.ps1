# Windows x64/MSVC bootstrap. No machine changes unless -Install is supplied.
[CmdletBinding()]
param(
    [switch]$Install,
    [switch]$Mermaid,
    [switch]$Installer,
    [ValidatePattern('^6\.[0-9]+\.[0-9]+$')][string]$QtVersion = '6.12.0',
    [string]$QtRoot = "$env:LOCALAPPDATA\Write\Qt"
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$missing = [Collections.Generic.List[string]]::new()
function Refresh-Path {
    $env:PATH = [Environment]::GetEnvironmentVariable('Path', 'Machine') + ';' +
        [Environment]::GetEnvironmentVariable('Path', 'User') + ';' + $env:PATH
}
function Has-Command([string]$Name) { return [bool](Get-Command $Name -ErrorAction SilentlyContinue) }
function Ensure-Package([string]$Id, [scriptblock]$Probe, [string[]]$Extra = @()) {
    if (& $Probe) { Write-Host "OK: $Id"; return }
    if (!$Install) { $missing.Add($Id); return }
    if (!(Has-Command 'winget.exe')) { throw 'Install Microsoft App Installer (winget) first.' }
    & winget.exe install --id $Id --exact --source winget --accept-package-agreements --accept-source-agreements @Extra
    if ($LASTEXITCODE -ne 0) { throw "winget failed for $Id (exit $LASTEXITCODE)." }
    Refresh-Path
    if (!(& $Probe)) { throw "$Id is not ready. Finish installation, reopen CMD and retry." }
}
try {
    Refresh-Path
    if (![Environment]::Is64BitOperatingSystem -or $env:PROCESSOR_ARCHITEW6432 -eq 'ARM64' -or $env:PROCESSOR_ARCHITECTURE -eq 'ARM64') {
        throw 'Automatic setup supports Windows x64. For other targets use a matching Qt/compiler kit; see docs/install.md.'
    }
    Ensure-Package 'Git.Git' { Has-Command 'git.exe' }
    Ensure-Package 'Kitware.CMake' { (Has-Command 'cmake.exe') -and [version]((& cmake.exe --version | Select-Object -First 1) -split ' ')[2] -ge [version]'3.24' }
    Ensure-Package 'Ninja-build.Ninja' { Has-Command 'ninja.exe' }
    Ensure-Package 'StrawberryPerl.StrawberryPerl' {
        if (!(Has-Command 'perl.exe')) { return $false }
        & perl.exe -e 'exit($^O =~ /MSWin32/ ? 0 : 1)'
        return $LASTEXITCODE -eq 0
    }
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    Ensure-Package 'Microsoft.VisualStudio.2022.BuildTools' {
        if (!(Test-Path -LiteralPath $vswhere)) { return $false }
        return [bool](& $vswhere -latest -products '*' -version '[17.0,18.0)' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath)
    } @('--override', '--wait --passive --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended')
    $sdkHeaders = "${env:ProgramFiles(x86)}\Windows Kits\10\Include\*\um\spellcheck.h"
    if (!(Test-Path -Path $sdkHeaders)) {
        if ($Install) { throw 'Windows SDK headers missing. Modify Visual Studio Build Tools: enable the C++ workload and Windows 10/11 SDK, then retry.' }
        $missing.Add('Windows 10/11 SDK (C++ workload; spellcheck.h)')
    }
    Ensure-Package 'Microsoft.VCRedist.2015+.x64' {
        (Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\VisualStudio\14.0\VC\Runtimes\x64' -ErrorAction SilentlyContinue).Installed -eq 1
    }
    $qtDir = if ($env:QT_DIR) { $env:QT_DIR } else { Join-Path $QtRoot "$QtVersion\msvc2022_64" }
    # Reuse a previously recorded kit across application/tool-directory renames.
    $recordedSetup = Join-Path $root 'build\windows-setup.json'
    if (!$env:QT_DIR -and !$PSBoundParameters.ContainsKey('QtRoot') -and !$PSBoundParameters.ContainsKey('QtVersion') -and (Test-Path -LiteralPath $recordedSetup)) {
        $recordedQt = (Get-Content -LiteralPath $recordedSetup -Raw | ConvertFrom-Json).QtDir
        if ($recordedQt -and (Test-Path -LiteralPath (Join-Path $recordedQt 'bin\qmake.exe'))) { $qtDir = $recordedQt }
    }
    if (!(Test-Path -LiteralPath (Join-Path $qtDir 'bin\qmake.exe'))) {
        if (!$Install) { $missing.Add("Qt $QtVersion MSVC 2022 x64 (or set QT_DIR)") }
        elseif ($env:QT_DIR) { throw "QT_DIR does not contain bin\qmake.exe: $qtDir" }
        else {
            Ensure-Package 'Python.Python.3.12' {
                if (!(Has-Command 'py.exe')) { return $false }
                try {
                    & py.exe -3.12 -c 'import sys' 2>$null
                    return $LASTEXITCODE -eq 0
                } catch { return $false }
            }
            $venv = Join-Path $root '.deps\aqt'
            if (!(Test-Path -LiteralPath "$venv\Scripts\python.exe")) {
                & py.exe -3.12 -m venv $venv
                if ($LASTEXITCODE -ne 0) { throw 'Could not create the aqt virtual environment.' }
            }
            & "$venv\Scripts\python.exe" -m pip install --upgrade git+https://github.com/miurahr/aqtinstall.git
            if ($LASTEXITCODE -ne 0) { throw 'Could not install aqtinstall.' }
            & "$venv\Scripts\python.exe" -m aqt install-qt windows desktop $QtVersion win64_msvc2022_64 --outputdir $QtRoot
            if ($LASTEXITCODE -ne 0) { throw 'Qt download failed. Check archive availability, network and Qt licensing.' }
        }
    }
    if ($Mermaid) {
        Ensure-Package 'OpenJS.NodeJS.LTS' { (Has-Command 'node.exe') -and (Has-Command 'npm.cmd') }
        if ($Install) {
            & npm.cmd install --global '@mermaid-js/mermaid-cli'
            if ($LASTEXITCODE -ne 0) { throw 'Mermaid CLI/browser installation failed.' }
        } elseif (!(Test-Path -LiteralPath "$env:APPDATA\npm\mmdc.cmd") -and !(Has-Command 'mmdc.cmd')) {
            $missing.Add('Mermaid CLI: npm install --global @mermaid-js/mermaid-cli')
        }
    }
    if ($Installer) {
        Ensure-Package 'JRSoftware.InnoSetup' {
            $candidates = @("${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe", "$env:ProgramFiles\Inno Setup 6\ISCC.exe", "$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe")
            if (Has-Command 'ISCC.exe') { $candidates = @((Get-Command 'ISCC.exe').Source) + $candidates }
            foreach ($candidate in $candidates) {
                if (Test-Path -LiteralPath $candidate) {
                    & (Join-Path $PSScriptRoot 'check-inno-compiler.ps1') -Compiler $candidate -Quiet
                    if ($LASTEXITCODE -eq 0) { return $true }
                }
            }
            return $false
        }
    }
    if ($missing.Count) {
        Write-Host "Missing prerequisites:`n - $($missing -join "`n - ")"
        Write-Host 'Run setup.bat -Install (optionally -Mermaid -Installer). Qt and third-party licenses apply.'
        exit 1
    }
    $qmake = Join-Path $qtDir 'bin\qmake.exe'
    $version = & $qmake -query QT_VERSION
    if ($LASTEXITCODE -ne 0 -or [version]$version -lt [version]'6.7') { throw 'Qt 6.7+ is required.' }
    $kit = & $qmake -query QMAKE_XSPEC
    if ($LASTEXITCODE -ne 0 -or $kit -ne 'win32-msvc') { throw 'Automatic setup requires an MSVC desktop Qt kit. MinGW remains available through build.bat.' }
    $archData = & $qmake -query QT_INSTALL_ARCHDATA
    if ($LASTEXITCODE -ne 0) { throw 'Could not inspect Qt architecture.' }
    $config = [IO.File]::ReadAllText((Join-Path $archData 'mkspecs\qconfig.pri'))
    if ([regex]::Match($config, '(?m)^\s*QT_ARCH\s*=\s*(\S+)').Groups[1].Value -ne 'x86_64') {
        throw 'Automatic setup requires an x64 Qt kit; do not mix ARM64/x86 Qt with the x64 compiler.'
    }
    if ($Install) {
        New-Item -ItemType Directory -Force (Join-Path $root 'build') | Out-Null
        @{ QtDir = $qtDir } | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $root 'build\windows-setup.json') -Encoding UTF8
    }
    Write-Host "Qt: $qtDir ($version); MSVC 2022 x64 + Windows SDK required."
    Write-Host "Next in CMD: set `"QT_DIR=$qtDir`""
    Write-Host 'Then: build.bat --check, or build.bat --test --deploy (add --installer for an unsigned x64 setup EXE).'
    Write-Host 'Reopen CMD/the app after installation so GUI PATH changes take effect. No documents were changed.'
    exit 0
} catch { Write-Error $_; exit 1 }
