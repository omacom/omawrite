# Run with powershell -NoProfile -File tests/windows-packaging.ps1 (or pwsh).
# Uses fake app/compiler helpers. On hosted Windows, also compiles the actual
# installer script with real Inno for both kits; NEVER executes an installer.
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$temp = Join-Path ([IO.Path]::GetTempPath()) ('omawrite packaging ' + [guid]::NewGuid())
$engine = (Get-Process -Id $PID).Path
$real = $null
$previousStamp = $env:OMAWRITE_STAMP
$previousSignature = $env:OMAWRITE_SIGNATURE
$previousIscc = $env:ISCC
$previousReject = $env:OMAWRITE_TEST_REJECT_PROBE
$previousVersionFailure = $env:OMAWRITE_TEST_VERSION_FAILURE
try {
    foreach ($file in @('bin/setup-windows.ps1', 'bin/check-inno-compiler.ps1', 'bin/package-windows.ps1', 'tests/windows-packaging.ps1')) {
        $tokens = $null; $errors = $null
        [void][Management.Automation.Language.Parser]::ParseFile((Join-Path $root $file), [ref]$tokens, [ref]$errors)
        if ($errors) { throw ($errors | Out-String) }
    }
    New-Item -ItemType Directory -Path "$temp/dist/notices" -Force | Out-Null
    # The upstream installer must not share downstream install/shortcut paths.
    $installer = Get-Content -LiteralPath (Join-Path $root 'packaging/windows.iss') -Raw
    foreach ($line in @(
        'AppId={{752B3E5A-F81B-52B2-B78D-C00D485A4CFB}',
        'AppPublisherURL=https://github.com/omacom/omawrite',
        'DefaultDirName={localappdata}\Programs\Omawrite',
        'DefaultGroupName=Omawrite',
        'Name: "{autodesktop}\Omawrite"; Filename: "{app}\omawrite.exe"; Tasks: desktopicon'
    )) {
        if ($installer -notmatch ('(?m)^' + [regex]::Escape($line) + '\r?$')) {
            throw ('Unexpected upstream installer identity/path: ' + $line)
        }
    }
    # Exercise the actual build.bat toolchain guard.
    $batch = Get-Content -LiteralPath (Join-Path $root 'build.bat') -Raw
    $stampCommand = [regex]::Match($batch, '(?m)^powershell -NoProfile -Command "([^"\r\n]*\$env:OMAWRITE_STAMP[^"\r\n]*)"\r?$').Groups[1].Value
    if (!$stampCommand) { throw 'Missing toolchain stamp guard' }
    $env:OMAWRITE_STAMP = Join-Path $temp 'omawrite-toolchain.txt'
    $env:OMAWRITE_SIGNATURE = 'fake qmake|Qt|compiler|version|x64'
    if ($env:OS -eq 'Windows_NT') {
        if ($batch -match "(?<!`r)`n") { throw 'build.bat must have CRLF endings; re-check out the file using .gitattributes.' }
        # Keep the full real batch body (not just its PowerShell payload), so
        # CMD itself must resolve and CALL the toolchain subroutine label.
        $stampBatch = Join-Path $temp 'toolchain-guard.cmd'
        $entry = "@echo off`r`nsetlocal EnableExtensions DisableDelayedExpansion`r`ncall :checkToolchain`r`nexit /b %errorlevel%`r`n"
        [IO.File]::WriteAllText($stampBatch, $entry + $batch, [Text.Encoding]::ASCII)
    }
    function Invoke-StampCheck {
        $ErrorActionPreference = 'Continue'
        if ($env:OS -eq 'Windows_NT') { & $env:ComSpec /d /c $stampBatch 2>$null }
        else { & $engine -NoProfile -Command $stampCommand 2>$null }
        return $LASTEXITCODE
    }
    if ((Invoke-StampCheck) -ne 0) { throw 'Fresh stamp failed' }
    if ((Invoke-StampCheck) -ne 0) { throw 'Matching current stamp was rejected' }
    Set-Content -LiteralPath $env:OMAWRITE_STAMP -Value 'different new compiler'
    if ((Invoke-StampCheck) -eq 0) { throw 'Current toolchain mismatch bypassed guard' }
    Set-Content -LiteralPath "$temp/dist/notices/COPYRIGHT.md" -Value 'test notice'
    $helper = Join-Path $temp 'helper.exe'
    Remove-Item Env:OMAWRITE_TEST_REJECT_PROBE -ErrorAction SilentlyContinue
    Remove-Item Env:OMAWRITE_TEST_VERSION_FAILURE -ErrorAction SilentlyContinue
    if ($env:OS -eq 'Windows_NT') {
        Add-Type -OutputAssembly $helper -OutputType ConsoleApplication -TypeDefinition @'
using System;
using System.IO;
[assembly: System.Reflection.AssemblyFileVersion("0.0.0.0")]
public static class PackagingHelper {
    public static int Main(string[] args) {
        if (args.Length == 1 && args[0] == "-") {
            string probe = Console.In.ReadToEnd();
            if (!probe.Contains("Output=no") || !probe.Contains("OutputDir=\n") && !probe.Contains("OutputDir=\r\n") ||
                !probe.Contains("ArchitecturesAllowed=x64compatible") || Environment.GetEnvironmentVariable("OMAWRITE_TEST_REJECT_PROBE") == "1") return 2;
            Console.WriteLine("Compiler engine version: Inno Setup 6.3.0"); return 0;
        }
        if (args.Length == 1 && args[0] == "--version") {
            Console.WriteLine("omawrite 1.2.3"); return 0;
        }
        string output = null; bool runtime = false;
        foreach (string a in args) {
            if (a.StartsWith("/DOutputDir=")) output = a.Substring(12);
            if (a == "/DNeedsVCRuntime=" + Environment.GetEnvironmentVariable("OMAWRITE_TEST_RUNTIME")) runtime = true;
        }
        if (output == null || !runtime) return 2;
        Directory.CreateDirectory(output);
        File.WriteAllText(Path.Combine(output, "Write-1.2.3-windows-x64-setup.exe"), "fake installer");
        return 0;
    }
}
'@
        # Run the actual CMD discovery routine, not a PowerShell reimplementation.
        $findIscc = [regex]::Match($batch, '(?ms)^:findIscc\r?\n.*?(?=^:installer\r?$)').Value
        if (!$findIscc) { throw 'Missing compiler discovery routine' }
        $probe = Join-Path $temp 'find-iscc.cmd'
        $body = "@echo off`nsetlocal EnableExtensions DisableDelayedExpansion`nset ARCH=x64`nset `"ROOT=$root\`"`ncall :findIscc`nexit /b %errorlevel%`n" + $findIscc
        $body.Replace("`r`n", "`n").Replace("`n", "`r`n") | Set-Content -LiteralPath $probe -Encoding ASCII
        # A malformed IF must never dispatch our real setup script by accident.
        @('@echo off', 'echo unexpected>setup-was-called.txt', 'exit /b 73') |
            Set-Content -LiteralPath (Join-Path $temp 'setup.bat') -Encoding ASCII
        $compilerDir = Join-Path $temp 'Inno Setup 6'
        New-Item -ItemType Directory -Path $compilerDir | Out-Null
        $compiler = Join-Path $compilerDir 'ISCC.exe'
        Copy-Item -LiteralPath $helper -Destination $compiler
        Push-Location $temp
        try {
            foreach ($value in @($compiler, ('"' + $compiler + '"'))) {
                $env:ISCC = $value
                & $env:ComSpec /d /c $probe
                if ($LASTEXITCODE -ne 0) { throw "CMD rejected compiler path: $value" }
            }
            $env:ISCC = '"' + (Join-Path $temp 'Missing Inno Setup 6\ISCC.exe') + '"'
            $ErrorActionPreference = 'Continue'
            & $env:ComSpec /d /c $probe >$null 2>$null
            $code = $LASTEXITCODE
            $ErrorActionPreference = 'Stop'
            if ($code -eq 0) { throw 'Missing compiler was not refused' }
            if (Test-Path -LiteralPath 'setup-was-called.txt') { throw 'Compiler discovery accidentally executed setup.bat' }
            if ($env:GITHUB_ACTIONS -eq 'true') {
                # The Windows 2022 image includes Inno Setup. Exercise the real
                # compiler as well: fake file versions missed this regression.
                $real = @("${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe", "$env:ProgramFiles\Inno Setup 6\ISCC.exe") |
                    Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
                if (!$real) { throw 'Expected the hosted Windows image to provide Inno Setup.' }
                $env:ISCC = $real
                & $env:ComSpec /d /c $probe
                if ($LASTEXITCODE -ne 0) { throw 'The installed real Inno compiler failed the capability check.' }
                & (Join-Path $root 'bin/check-inno-compiler.ps1') -Compiler $real -Quiet
                if ($LASTEXITCODE -ne 0) { throw 'The installed real Inno compiler failed the in-process setup check.' }
                Write-Host 'PASS: real hosted Inno compiler syntax-only probes, separate-process and in-process; no installer created or executed.'
            }
            Write-Host 'PASS: native CMD compiler discovery accepts quoted/unquoted zero-file-version paths and safely rejects missing compilers.'
        } finally { Pop-Location }
    } else {
        # Portable mock permits local validation on macOS/Linux without Windows tools.
        @'
#!/usr/bin/env python3
import os, sys
if sys.argv[1:] == ['-']:
    probe = sys.stdin.read()
    assert 'Output=no' in probe and 'OutputDir=\n' in probe
    assert 'ArchitecturesAllowed=x64compatible' in probe
    assert os.environ.get('OMAWRITE_TEST_REJECT_PROBE') != '1'
    print('Compiler engine version: Inno Setup 6.3.0')
elif sys.argv[1:] == ['--version']:
    if os.environ.get('OMAWRITE_TEST_VERSION_FAILURE') == '1':
        print('Simulated version failure', file=sys.stderr)
        sys.exit(42)
    print('omawrite 1.2.3')
else:
    assert '/DNeedsVCRuntime=' + os.environ['OMAWRITE_TEST_RUNTIME'] in sys.argv
    path = next(a[12:] for a in sys.argv if a.startswith('/DOutputDir='))
    os.makedirs(path, exist_ok=True)
    open(os.path.join(path, 'Write-1.2.3-windows-x64-setup.exe'), 'w').write('fake installer')
'@ | Set-Content -LiteralPath $helper -Encoding ASCII
        & chmod +x $helper
        if ($LASTEXITCODE -ne 0) { throw 'Could not mark mock executable.' }
    }
    $checker = Join-Path $root 'bin/check-inno-compiler.ps1'
    & $engine -NoProfile -File $checker -Compiler $helper
    if ($LASTEXITCODE -ne 0) { throw 'Supported compiler capability probe failed' }
    # setup invokes this script in-process; -File in a fresh host hides local
    # LASTEXITCODE shadowing. Exercise its exact probe against a per-user kit.
    function Test-SetupInnoProbe {
        $names = @('LOCALAPPDATA', 'ProgramFiles', 'ProgramFiles(x86)')
        $saved = @{}
        foreach ($name in $names) { $saved[$name] = [Environment]::GetEnvironmentVariable($name, 'Process') }
        try {
            $env:LOCALAPPDATA = Join-Path $temp 'fake local profile'
            $env:ProgramFiles = Join-Path $temp 'empty program files'
            ${env:ProgramFiles(x86)} = Join-Path $temp 'empty program files x86'
            $directory = Join-Path $env:LOCALAPPDATA 'Programs\Inno Setup 6'
            New-Item -ItemType Directory -Path $directory -Force | Out-Null
            Copy-Item -LiteralPath $helper -Destination (Join-Path $directory 'ISCC.exe')
            function Has-Command([string]$Name) { return $false }
            $tokens = $null; $errors = $null
            $ast = [Management.Automation.Language.Parser]::ParseFile((Join-Path $root 'bin/setup-windows.ps1'), [ref]$tokens, [ref]$errors)
            $command = $ast.Find({ param($node)
                $node -is [Management.Automation.Language.CommandAst] -and
                $node.GetCommandName() -eq 'Ensure-Package' -and
                $node.CommandElements.Count -gt 2 -and
                $node.CommandElements[1].Extent.Text -eq "'JRSoftware.InnoSetup'"
            }, $true)
            if (!$command) { throw 'Missing setup Inno probe' }
            $probe = $command.CommandElements[2].ScriptBlock.GetScriptBlock()
            $global:LASTEXITCODE = 37
            if (!(& $probe)) { throw 'Setup failed to detect a working per-user Inno compiler' }
            $env:OMAWRITE_TEST_REJECT_PROBE = '1'
            if (& $probe) { throw 'Setup accepted a compiler that rejects required features' }
            Remove-Item Env:OMAWRITE_TEST_REJECT_PROBE
            Write-Host 'PASS: actual setup probe detects per-user Inno in-process and rejects unsupported compilers.'
        } finally {
            foreach ($name in $names) { [Environment]::SetEnvironmentVariable($name, $saved[$name], 'Process') }
        }
    }
    Test-SetupInnoProbe
    $env:OMAWRITE_TEST_REJECT_PROBE = '1'
    $ErrorActionPreference = 'Continue'
    & $engine -NoProfile -File $checker -Compiler $helper >$null 2>$null
    $code = $LASTEXITCODE
    $ErrorActionPreference = 'Stop'
    Remove-Item Env:OMAWRITE_TEST_REJECT_PROBE
    if ($code -eq 0) { throw 'Unsupported compiler was not refused' }
    $appHelper = $helper
    if ($env:OS -eq 'Windows_NT') {
        # Match the real application's PE subsystem. A console helper masks
        # PowerShell's asynchronous GUI invocation and missing exit-code bug.
        $appHelper = Join-Path $temp 'gui-version.exe'
        Add-Type -OutputAssembly $appHelper -OutputType WindowsApplication -TypeDefinition @'
using System;
using System.Threading;
public static class GuiVersionHelper {
    public static int Main(string[] args) {
        if (args.Length != 1 || args[0] != "--version") return 3;
        Thread.Sleep(300);
        if (Environment.GetEnvironmentVariable("OMAWRITE_TEST_VERSION_FAILURE") == "1") {
            Console.Error.WriteLine("Simulated version failure"); return 42;
        }
        Console.WriteLine("omawrite 1.2.3"); return 0;
    }
}
'@
        $pe = [IO.File]::ReadAllBytes($appHelper)
        $peOffset = [BitConverter]::ToInt32($pe, 60)
        if ([BitConverter]::ToUInt16($pe, $peOffset + 92) -ne 2) { throw 'Version fixture must use the Windows GUI subsystem' }
    }
    Copy-Item -LiteralPath $appHelper -Destination "$temp/dist/omawrite.exe"
    $packager = Join-Path $root 'bin/package-windows.ps1'
    foreach ($kit in @('win32-msvc', 'win32-g++')) {
        $env:OMAWRITE_TEST_RUNTIME = if ($kit -eq 'win32-msvc') { '1' } else { '0' }
        & $engine -NoProfile -File $packager -DistDir "$temp/dist" -Iscc $helper -Kit $kit
        if ($LASTEXITCODE -ne 0) { throw "Packaging failed for $kit" }
        $artifact = "$temp/installer/Write-1.2.3-windows-x64-setup.exe"
        if (!(Test-Path -LiteralPath $artifact)) { throw 'Missing expected artifact' }
        # PS 5.1 can promote redirected native stderr under Stop: these failures are expected.
        $ErrorActionPreference = 'Continue'
        & $engine -NoProfile -File $packager -DistDir "$temp/dist" -Iscc $helper -Kit $kit 2>$null
        $code = $LASTEXITCODE
        $ErrorActionPreference = 'Stop'
        if ($code -eq 0) { throw 'Existing installer was not refused' }
        Remove-Item -LiteralPath $artifact
        if ($real) {
            # Compile our actual .iss, not just the capability probe. CLI /D
            # definitions are strings; fake compilers cannot detect type errors.
            & $engine -NoProfile -File $packager -DistDir "$temp/dist" -Iscc $real -Kit $kit
            if ($LASTEXITCODE -ne 0) { throw "Real Inno compilation failed for $kit" }
            if (!(Test-Path -LiteralPath $artifact) -or (Get-Item -LiteralPath $artifact).Length -eq 0) {
                throw "Real Inno did not produce the expected $kit installer"
            }
            Write-Host "PASS: actual windows.iss compiled with real Inno for $kit; installer not executed."
            Remove-Item -LiteralPath $artifact
        }
    }
    $env:OMAWRITE_TEST_VERSION_FAILURE = '1'
    $ErrorActionPreference = 'Continue'
    $failure = & $engine -NoProfile -File $packager -DistDir "$temp/dist" -Iscc $helper -Kit win32-msvc 2>&1 | Out-String
    $code = $LASTEXITCODE
    $ErrorActionPreference = 'Stop'
    Remove-Item Env:OMAWRITE_TEST_VERSION_FAILURE
    # PowerShell's error formatter may wrap words with line-number gutters.
    if ($code -eq 0 -or $failure -notmatch '0x0000002A' -or $failure -notmatch 'Simulated') {
        throw "Failed version query was not refused with exit/output diagnostics: $failure"
    }
    if (Test-Path -LiteralPath "$temp/installer/Write-1.2.3-windows-x64-setup.exe") { throw 'Version failure still produced an installer' }
    Write-Host 'PASS: synchronous application version capture and fail-closed exit/output diagnostics (GUI-subsystem app on Windows).'
    Remove-Item -LiteralPath "$temp/dist/notices/COPYRIGHT.md"
    $ErrorActionPreference = 'Continue'
    & $engine -NoProfile -File $packager -DistDir "$temp/dist" -Iscc $helper -Kit win32-msvc 2>$null
    $code = $LASTEXITCODE
    $ErrorActionPreference = 'Stop'
    if ($code -eq 0) { throw 'Missing deployment notices were not refused' }
    Write-Host 'PASS: script parsing, current/legacy toolchain stamps, MSVC/MinGW flags, spaces in paths, artifact naming, overwrite and incomplete-deployment guards.'
} finally {
    [Environment]::SetEnvironmentVariable('OMAWRITE_STAMP', $previousStamp, 'Process')
    [Environment]::SetEnvironmentVariable('OMAWRITE_SIGNATURE', $previousSignature, 'Process')
    [Environment]::SetEnvironmentVariable('ISCC', $previousIscc, 'Process')
    [Environment]::SetEnvironmentVariable('OMAWRITE_TEST_REJECT_PROBE', $previousReject, 'Process')
    [Environment]::SetEnvironmentVariable('OMAWRITE_TEST_VERSION_FAILURE', $previousVersionFailure, 'Process')
    Remove-Item Env:OMAWRITE_TEST_RUNTIME -ErrorAction SilentlyContinue
    if (Test-Path -LiteralPath $temp) { Remove-Item -LiteralPath $temp -Recurse -Force }
}
# Expected failing child probes leave LASTEXITCODE nonzero. Report our own
# success explicitly, including to GitHub's Windows PowerShell wrapper.
exit 0
