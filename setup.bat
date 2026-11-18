@echo off
rem Windows entry points are checked out with CRLF through .gitattributes.
setlocal
rem Explicit -Install opts into downloads and package-manager/UAC prompts.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0bin\setup-windows.ps1" %*
exit /b %errorlevel%
