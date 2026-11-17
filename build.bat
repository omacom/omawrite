@echo off
rem Keep CRLF line endings: cmd.exe CALL/GOTO label scanning relies on them.
setlocal EnableExtensions DisableDelayedExpansion
rem Native Windows build. See docs/install.md; setup.bat -Install bootstraps x64 MSVC.
set "ROOT=%~dp0"
rem Ignore inherited compiler/Perl overrides: resolve this kit's tools explicitly.
set "CXX="
set "PERL_EXE="
set "DEPLOY=0"
set "RUN_TESTS=0"
set "CHECK_ONLY=0"
set "INSTALLER=0"
:options
if "%~1"=="" goto setup
if /i "%~1"=="--deploy" (
    set "DEPLOY=1"
    shift
    goto options
)
if /i "%~1"=="--test" (
    set "RUN_TESTS=1"
    shift
    goto options
)
if /i "%~1"=="--check" (
    set "CHECK_ONLY=1"
    shift
    goto options
)
if /i "%~1"=="--installer" (
    set "INSTALLER=1"
    set "DEPLOY=1"
    shift
    goto options
)
if /i "%~1"=="--help" goto help
echo ERROR: Unknown argument "%~1".
goto helpError

:setup
if not defined JOBS set "JOBS=%NUMBER_OF_PROCESSORS%"
if not defined JOBS set "JOBS=4"
powershell -NoProfile -Command "if ($env:JOBS -notmatch '^[1-9][0-9]*$') { exit 1 }"
if errorlevel 1 goto badJobs
rem A successful setup may record its kit locally; explicit environment wins.
if not defined QT_DIR if not defined QMAKE if exist "%ROOT%build\windows-setup.json" for /f "delims=" %%I in ('powershell -NoProfile -Command "(Get-Content -LiteralPath ($env:ROOT+'build\windows-setup.json') -Raw | ConvertFrom-Json).QtDir"') do set "QT_DIR=%%I"
if defined QT_DIR set "QMAKE=%QT_DIR%\bin\qmake.exe"
if defined QMAKE goto haveQmake
for /f "delims=" %%I in ('where qmake6.exe 2^>nul') do if not defined QMAKE set "QMAKE=%%I"
for /f "delims=" %%I in ('where qmake.exe 2^>nul') do if not defined QMAKE set "QMAKE=%%I"
:haveQmake
if not defined QMAKE goto missingQt
if not exist "%QMAKE%" goto missingQt
for /f "delims=" %%I in ('""%QMAKE%" -query QT_INSTALL_PREFIX"') do set "QT_PREFIX=%%I"
for /f "delims=" %%I in ('""%QMAKE%" -query QT_VERSION"') do set "QT_VERSION=%%I"
for /f "delims=" %%I in ('""%QMAKE%" -query QMAKE_XSPEC"') do set "KIT=%%I"
powershell -NoProfile -Command "try { if ([version]$env:QT_VERSION -lt [version]'6.7') { exit 1 } } catch { exit 1 }"
if errorlevel 1 goto missingQt
set "PATH=%QT_PREFIX%\bin;%PATH%"
if "%KIT%"=="win32-msvc" goto msvc
if "%KIT%"=="win32-g++" goto mingw
echo ERROR: Unsupported Qt kit "%KIT%". Use a Windows desktop MSVC or MinGW kit.
goto fail

:msvc
where cl.exe >nul 2>&1
if errorlevel 1 call :initMsvc
if errorlevel 1 goto missingCompiler
call :require cl.exe
if errorlevel 1 goto missingCompiler
call :require nmake.exe
if errorlevel 1 goto missingCompiler
if not defined VSCMD_ARG_TGT_ARCH goto missingCompiler
set "ARCH=%VSCMD_ARG_TGT_ARCH%"
rem Qt rejects a 32-bit compiler with a 64-bit kit as a version mismatch.
rem Read the kit architecture instead of guessing from its directory name.
set "QT_ARCHDATA="
for /f "delims=" %%I in ('""%QMAKE%" -query QT_INSTALL_ARCHDATA"') do set "QT_ARCHDATA=%%I"
powershell -NoProfile -Command "$ErrorActionPreference='Stop'; $p=Join-Path $env:QT_ARCHDATA 'mkspecs/qconfig.pri'; $m=[regex]::Match([IO.File]::ReadAllText($p),'(?m)^\s*QT_ARCH\s*=\s*(\S+)'); $targets=@{x86_64='x64'; i386='x86'; x86='x86'; arm64='arm64'; aarch64='arm64'}; $target=$targets[$m.Groups[1].Value]; if (!$target) { Write-Error ('Cannot determine Qt target architecture from '+$p); exit 1 }; if ($target -ne $env:ARCH) { Write-Host ('ERROR: Qt targets '+$target+' but this MSVC prompt targets '+$env:ARCH+'.'); Write-Host ('Run VsDevCmd.bat -arch='+$target+' -host_arch=x64, then retry.'); exit 1 }"
if errorlevel 1 goto fail
for /f "delims=" %%I in ('where cl.exe') do if not defined CXX set "CXX=%%I"
set "MAKE=nmake.exe"
goto tools

:mingw
rem Strawberry Perl contains GCC too: explicitly put the Qt compiler first.
if not defined MINGW_DIR (
    echo ERROR: Set MINGW_DIR to the matching Qt-supplied MinGW directory, without \bin.
    goto fail
)
set "PATH=%MINGW_DIR%\bin;%PATH%"
if not exist "%MINGW_DIR%\bin\g++.exe" goto missingCompiler
if not exist "%MINGW_DIR%\bin\mingw32-make.exe" goto missingCompiler
set "CXX=%MINGW_DIR%\bin\g++.exe"
set "MAKE=%MINGW_DIR%\bin\mingw32-make.exe"
for /f "delims=" %%I in ('""%CXX%" -dumpmachine"') do set "ARCH=%%I"

:tools
call :require cmake.exe
if errorlevel 1 goto fail
call :require ninja.exe
if errorlevel 1 goto fail
call :require perl.exe
if errorlevel 1 goto missingPerl
cmake --version
powershell -NoProfile -Command "$v = ((cmake --version | Select-Object -First 1) -split ' ')[2]; if ([version]$v -lt [version]'3.24') { exit 1 }"
if errorlevel 1 goto fail
perl -e "exit($^O eq 'MSWin32' ? 0 : 1)"
if errorlevel 1 goto missingPerl
for /f "delims=" %%I in ('where perl.exe') do if not defined PERL_EXE set "PERL_EXE=%%I"
if "%INSTALLER%"=="1" call :findIscc
if errorlevel 1 goto fail
if "%CHECK_ONLY%"=="1" (
    echo CHECK OK: Qt %QT_VERSION%, %KIT%, target %ARCH%.
    echo Compiler: "%CXX%". No dependency build or deployment performed.
    exit /b 0
)
set "CMAKE_BUILD_PARALLEL_LEVEL=%JOBS%"
set "DEPS=%ROOT%.deps\windows-%KIT%-qt%QT_VERSION%-%ARCH%"
set "APP_BUILD=%ROOT%build\windows-%KIT%-qt%QT_VERSION%-%ARCH%"
set "DIST=%APP_BUILD%\dist"
rem These Windows-specific directories never reuse a macOS/Linux dependency cache.
set "OMAWRITE_SYNTAX_PREFIX=%DEPS%\install"
set "OMAWRITE_SPELL_PREFIX=%DEPS%\install"
set "OMAWRITE_SIGNATURE=%QMAKE%|%QT_PREFIX%|%CXX%|%VCToolsVersion%|%ARCH%"
set "OMAWRITE_STAMP=%DEPS%\omawrite-toolchain.txt"
call :checkToolchain
if errorlevel 1 goto fail
set "OMAWRITE_STAMP=%APP_BUILD%\omawrite-toolchain.txt"
call :checkToolchain
if errorlevel 1 goto fail

echo === Building pinned ECM, KSyntaxHighlighting and Sonnet ===
cmake -S "%ROOT%dependencies" -B "%DEPS%" -G Ninja "-DCMAKE_PREFIX_PATH:PATH=%QT_PREFIX%" "-DCMAKE_CXX_COMPILER:FILEPATH=%CXX%" "-DPERL_EXECUTABLE:FILEPATH=%PERL_EXE%" -DCMAKE_BUILD_TYPE:STRING=Release
if errorlevel 1 goto fail
cmake --build "%DEPS%" --config Release --parallel %JOBS%
if errorlevel 1 goto fail

echo === Building Write ===
pushd "%APP_BUILD%"
if errorlevel 1 goto fail
"%QMAKE%" "%ROOT%omawrite.pro" CONFIG+=release
if errorlevel 1 goto buildFailed
call :make
if errorlevel 1 goto buildFailed
set "APP_EXE=%APP_BUILD%\omawrite.exe"
if exist "%APP_BUILD%\release\omawrite.exe" set "APP_EXE=%APP_BUILD%\release\omawrite.exe"
if not exist "%APP_EXE%" goto buildFailed
popd
if "%RUN_TESTS%"=="1" call :tests
if errorlevel 1 goto fail
if "%DEPLOY%"=="1" call :deploy
if errorlevel 1 goto fail
if "%INSTALLER%"=="1" call :installer
if errorlevel 1 goto fail
echo.
echo SUCCESS: "%APP_EXE%"
if "%DEPLOY%"=="1" echo Deployment folder: "%DIST%" (MSVC targets also need the VC++ Redistributable)
exit /b 0

:make
if "%KIT%"=="win32-g++" (
    "%MAKE%" -j%JOBS%
) else (
    "%MAKE%" /NOLOGO
)
exit /b %errorlevel%

:tests
rem These cover Mermaid resolver/tree cleanup and the main native editor suite.
rem They do not install Node or browsers. Keep Qt bin on PATH while testing.
set "TEST_PROJECT=%ROOT%tests\mermaid\mermaid.pro"
set "TEST_DIR=%APP_BUILD%\tests-mermaid"
set "TEST_NAME=tst_mermaid.exe"
call :testProject
if errorlevel 1 exit /b 1
set "TEST_PROJECT=%ROOT%tests\tests.pro"
set "TEST_DIR=%APP_BUILD%\tests-main"
set "TEST_NAME=tst_editor.exe"
call :testProject
exit /b %errorlevel%

:testProject
if not exist "%TEST_DIR%" mkdir "%TEST_DIR%"
if errorlevel 1 exit /b 1
pushd "%TEST_DIR%"
if errorlevel 1 exit /b 1
"%QMAKE%" "%TEST_PROJECT%" CONFIG+=release
if errorlevel 1 goto testFailed
call :make
if errorlevel 1 goto testFailed
set "TEST_EXE=%TEST_DIR%\%TEST_NAME%"
if exist "%TEST_DIR%\release\%TEST_NAME%" set "TEST_EXE=%TEST_DIR%\release\%TEST_NAME%"
"%TEST_EXE%"
if errorlevel 1 goto testFailed
popd
exit /b 0
:testFailed
popd
exit /b 1

:deploy
if not exist "%QT_PREFIX%\bin\windeployqt.exe" exit /b 1
rem Do not silently remove a previous distribution or files placed in it by hand.
if exist "%DIST%" (
    echo ERROR: Deployment directory exists. Move or remove it before --deploy: "%DIST%"
    exit /b 1
)
mkdir "%DIST%"
if errorlevel 1 exit /b 1
copy /y "%APP_EXE%" "%DIST%\omawrite.exe" >nul
if errorlevel 1 exit /b 1
"%QT_PREFIX%\bin\windeployqt.exe" --release --compiler-runtime --qmldir "%ROOT%src" "%DIST%\omawrite.exe"
if errorlevel 1 exit /b 1
rem Preserve license notices; this is not a release/redistribution clearance.
powershell -NoProfile -Command "$ErrorActionPreference='Stop'; $d=Join-Path $env:DIST 'notices'; New-Item -ItemType Directory -Path $d | Out-Null; Copy-Item -LiteralPath ($env:ROOT+'LICENSE'),($env:ROOT+'COPYRIGHT.md') -Destination $d; foreach ($p in @('third_party','fonts')) { Copy-Item -LiteralPath (Join-Path $env:ROOT $p) -Destination $d -Recurse }"
exit /b %errorlevel%

:findIscc
if not "%ARCH%"=="x64" if not "%ARCH%"=="x86_64-w64-mingw32" (
    echo ERROR: The installer currently supports x64 targets only.
    exit /b 1
)
rem Accept both set ISCC="path" and set "ISCC=path". Embedded quotes would
rem break the IF EXIST argument and can accidentally execute setup.bat.
if defined ISCC set "ISCC=%ISCC:"=%"
if not defined ISCC for /f "delims=" %%I in ('where ISCC.exe 2^>nul') do if not defined ISCC set "ISCC=%%I"
if not defined ISCC if exist "%ProgramFiles(x86)%\Inno Setup 6\ISCC.exe" set "ISCC=%ProgramFiles(x86)%\Inno Setup 6\ISCC.exe"
if not defined ISCC if exist "%ProgramFiles%\Inno Setup 6\ISCC.exe" set "ISCC=%ProgramFiles%\Inno Setup 6\ISCC.exe"
if not defined ISCC if exist "%LOCALAPPDATA%\Programs\Inno Setup 6\ISCC.exe" set "ISCC=%LOCALAPPDATA%\Programs\Inno Setup 6\ISCC.exe"
if not defined ISCC goto missingIscc
if not exist "%ISCC%" goto missingIscc
powershell -NoProfile -ExecutionPolicy Bypass -File "%ROOT%bin\check-inno-compiler.ps1"
if errorlevel 1 exit /b 1
exit /b 0
:missingIscc
if defined ISCC echo ERROR: Inno Setup compiler not found at "%ISCC%".
if not defined ISCC echo ERROR: ISCC.exe was not found on PATH or in the standard Inno Setup folders.
echo Run setup.bat -Install -Installer, or set ISCC to the full ISCC.exe path.
exit /b 1

:installer
powershell -NoProfile -ExecutionPolicy Bypass -File "%ROOT%bin\package-windows.ps1" -DistDir "%DIST%" -Iscc "%ISCC%" -Kit "%KIT%"
exit /b %errorlevel%

:checkToolchain
rem Validate legacy stamps too, so a rename cannot bypass cache/toolchain safety.
powershell -NoProfile -Command "$ErrorActionPreference='Stop'; $p=$env:OMAWRITE_STAMP; if ((Test-Path -LiteralPath $p) -and ((Get-Content -LiteralPath $p -Raw).TrimEnd() -ne $env:OMAWRITE_SIGNATURE)) { Write-Error ('Toolchain changed. Move/remove this generated build directory: '+(Split-Path $p)); exit 1 }; New-Item -ItemType Directory -Force -Path (Split-Path $p) | Out-Null; [IO.File]::WriteAllText($p,$env:OMAWRITE_SIGNATURE)"
exit /b %errorlevel%

:require
where "%~1" >nul 2>&1
if errorlevel 1 (
    echo ERROR: Missing %~1 on PATH. Run setup.bat -Install; see docs\install.md.
    exit /b 1
)
exit /b 0

:initMsvc
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" exit /b 1
for /f "usebackq delims=" %%I in (`"%VSWHERE%" -latest -products * -version "[17.0,18.0)" -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS_INSTALL=%%I"
if not defined VS_INSTALL exit /b 1
rem Automatic setup is x64 only. Other architectures need a prepared Developer Prompt.
call "%VS_INSTALL%\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64
exit /b %errorlevel%

:buildFailed
popd
goto fail
:missingQt
echo ERROR: Set QT_DIR to a Qt 6.7+ desktop kit, e.g. C:\Qt\6.8.3\msvc2022_64.
goto fail
:missingCompiler
echo ERROR: Use the compiler matching your Qt kit. For MSVC, use an x64 VS 2022 Developer Prompt.
goto fail
:missingPerl
echo ERROR: Native Windows Perl is required to generate syntax definitions, even with tests disabled.
echo Install: winget install --id StrawberryPerl.StrawberryPerl --exact --source winget
echo Open a new terminal afterward; keep the Qt compiler ahead of Strawberry's GCC on PATH.
goto fail
:badJobs
echo ERROR: JOBS must be a positive integer.
goto fail
:help
 echo Usage: build.bat [--check] [--test] [--deploy] [--installer]
 echo Set QT_DIR to your Qt desktop kit. MinGW also needs MINGW_DIR.
 echo Optional: JOBS, or an absolute QMAKE path instead of QT_DIR.
 echo Builds private dependencies, then the Release application. --test runs two suites.
 echo --deploy creates a new Qt runtime folder; MSVC needs VC++ Redistributable.
 echo Mermaid stays external.
 echo --installer implies --deploy; Inno Setup 6 creates an unsigned x64 per-user installer.
 echo --check validates tools without building. setup.bat -Install -Installer installs prerequisites.
 echo Installation and manual Windows checks: docs\install.md
 exit /b 0
:helpError
call :help
:fail
echo BUILD FAILED. See the first error above. No successful Windows build is implied.
exit /b 1
